// clang-format off
/******************************************************************************
 * Copyright (c) 2014-2016, Pedro Ramalhete, Andreia Correia
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Concurrency Freaks nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ******************************************************************************
 */
// clang-format on

/**
 * Code is modified to allow caching HazardPointer record per thread to allow faster enqueue and deque by skipping hazard pointer record
 * acquire every time
 */

#ifndef _FAA_ARRAY_QUEUE_HP_H_
#define _FAA_ARRAY_QUEUE_HP_H_

#include "HazardPointers.hpp"
#include "CoPaTTypes.h"

#include <atomic>

COPAT_NS_INLINED
namespace copat
{

// clang-format off
/**
 * <h1> Fetch-And-Add Array Queue </h1>
 *
 * Each node has one array but we don't search for a vacant entry. Instead, we
 * use FAA to obtain an index in the array, for enqueueing or dequeuing.
 *
 * There are some similarities between this queue and the basic queue in YMC:
 * http://chaoran.me/assets/pdf/wfq-ppopp16.pdf
 * but it's not the same because the queue in listing 1 is obstruction-free, while
 * our algorithm is lock-free.
 * In FAAArrayQueue eventually a new node will be inserted (using Michael-Scott's
 * algorithm) and it will have an item pre-filled in the first position, which means
 * that at most, after BUFFER_SIZE steps, one item will be enqueued (and it can then
 * be dequeued). This kind of progress is lock-free.
 *
 * Each entry in the array may contain one of three possible values:
 * - A valid item that has been enqueued;
 * - nullptr, which means no item has yet been enqueued in that position;
 * - taken, a special value that means there was an item but it has been dequeued;
 *
 * Enqueue algorithm: FAA + CAS(null,item)
 * Dequeue algorithm: FAA + CAS(item,taken)
 * Consistency: Linearizable
 * enqueue() progress: lock-free
 * dequeue() progress: lock-free
 * Memory Reclamation: Hazard Pointers (lock-free)
 * Uncontended enqueue: 1 FAA + 1 CAS + 1 HP
 * Uncontended dequeue: 1 FAA + 1 CAS + 1 HP
 *
 *
 * <p>
 * Lock-Free Linked List as described in Maged Michael and Michael Scott's paper:
 * {@link http://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf}
 * <a href="http://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf">
 * Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms</a>
 * <p>
 * The paper on Hazard Pointers is named "Hazard Pointers: Safe Memory
 * Reclamation for Lock-Free objects" and it is available here:
 * http://web.cecs.pdx.edu/~walpole/class/cs510/papers/11.pdf
 *
 * @author Pedro Ramalhete
 * @author Andreia Correia
 */
// clang-format on
template <typename T>
class FAAArrayQueue
{
private:
    constexpr static const long BUFFER_SIZE = 1024; // 1024
    constexpr static const uintptr_t TAKEN_PTR = ~(uintptr_t)(0);

    struct Node
    {
        std::atomic<int> deqidx;
        std::atomic<T *> items[BUFFER_SIZE]; // items size is enough to separate deqidx and enqidx
        std::atomic<int> enqidx;
        std::atomic<Node *> next;

        // Start with the first entry pre-filled and enqidx at 1
        Node(T *item)
            : deqidx{ 0 }
            , enqidx{ 1 }
            , next{ nullptr }
        {
            items[0].store(item, std::memory_order_relaxed);
            for (long i = 1; i < BUFFER_SIZE; i++)
            {
                items[i].store(nullptr, std::memory_order_relaxed);
            }
        }

        bool casNext(Node *cmp, Node *val) { return next.compare_exchange_strong(cmp, val); }
    };

    bool casTail(Node *cmp, Node *val) { return tail.compare_exchange_strong(cmp, val); }

    bool casHead(Node *cmp, Node *val) { return head.compare_exchange_strong(cmp, val); }

    static T *takenPtr() { return (T *)TAKEN_PTR; }

    HazardPointersManager<Node> hazardsManager;

    // Pointers to head and tail of the list
    alignas(2 * CACHE_LINE_SIZE) std::atomic<Node *> head;
    alignas(2 * CACHE_LINE_SIZE) std::atomic<Node *> tail;

public:
    using HazardToken = HazardPointersManager<Node>::HazardPointer;

    FAAArrayQueue()
    {
        Node *sentinelNode = memNew<Node, Node *>(nullptr);
        sentinelNode->enqidx.store(0, std::memory_order_relaxed);
        head.store(sentinelNode, std::memory_order_relaxed);
        tail.store(sentinelNode, std::memory_order_relaxed);
    }

    ~FAAArrayQueue()
    {
        // Drain the queue
        while (dequeue() != nullptr)
        {}
        memDelete(head.load()); // Delete the last node
    }

    inline HazardToken getHazardToken() { return { hazardsManager }; }

    void enqueue(T *item)
    {
        HazardToken token{ hazardsManager };
        enqueue(item, token);
    }
    void enqueue(T *item, HazardToken &hazardRecord)
    {
        if (item == nullptr)
            return;
        while (true)
        {
            Node *ltail = hazardRecord.record->setHazardPtr(tail);
            const int idx = ltail->enqidx.fetch_add(1, std::memory_order::acq_rel);
            if (idx > BUFFER_SIZE - 1) // This node is full
            {
                if (ltail != tail.load(std::memory_order::acquire))
                    continue;
                // It is okay CAS ensures we are fine
                Node *lnext = ltail->next.load(std::memory_order::relaxed);
                if (lnext == nullptr)
                {
                    // Try acquire existing deleting node if any present
                    Node *newNode = hazardsManager.dequeueDelete();
                    newNode = newNode ? newNode : (Node *)(CoPaTMemAlloc::memAlloc(sizeof(Node), alignof(Node)));
                    newNode = new (newNode) Node(item);
                    if (ltail->casNext(nullptr, newNode))
                    {
                        casTail(ltail, newNode);
                        hazardRecord.record->reset();
                        return;
                    }
                    memDelete(newNode);
                }
                else
                {
                    casTail(ltail, lnext);
                }
                continue;
            }
            T *itemnull = nullptr;
            if (ltail->items[idx].compare_exchange_strong(itemnull, item))
            {
                hazardRecord.record->reset();
                return;
            }
        }
    }

    T *dequeue()
    {
        HazardToken token{ hazardsManager };
        return dequeue(token);
    }
    T *dequeue(HazardToken &hazardRecord)
    {
        while (true)
        {
            Node *lhead = hazardRecord.record->setHazardPtr(head);
            // Do not want to delete head node if it is same as tail
            if (lhead->deqidx.load(std::memory_order::acquire) >= lhead->enqidx.load(std::memory_order::acquire)
                && lhead->next.load(std::memory_order::acquire) == nullptr)
            {
                break;
            }

            const int idx = lhead->deqidx.fetch_add(1, std::memory_order::acq_rel);
            if (idx > BUFFER_SIZE - 1) // This node has been drained, check if there is another one
            {
                Node *lnext = lhead->next.load(std::memory_order::acquire);
                if (lnext == nullptr) // No more nodes in the queue
                {
                    break;
                }
                if (casHead(lhead, lnext))
                {
                    hazardRecord.record->reset();
                    hazardsManager.enqueueDelete(lhead);
                }
                continue;
            }
            T *item = lhead->items[idx].exchange(takenPtr(), std::memory_order::relaxed);
            if (item == nullptr)
            {
                continue;
            }
            hazardRecord.record->reset();
            return item;
        }
        hazardRecord.record->reset();
        return nullptr;
    }
};

/**
 * Just a modified version of FAAArrayQueue to allow multi producer but only single consumer
 * Dequeue when reaching end directly deletes
 * node
 * https://github.com/pramalhe/ConcurrencyFreaks/tree/master/CPP
 */
template <typename T>
class FAAArrayMPSCQueue
{
private:
    constexpr static const long BUFFER_SIZE = 1024; // 1024
    constexpr static const uintptr_t TAKEN_PTR = ~(uintptr_t)(0);

    struct Node
    {
        int deqidx;
        std::atomic<T *> items[BUFFER_SIZE]; // items size is enough to separate deqidx and enqidx
        std::atomic<int> enqidx;
        std::atomic<Node *> next;

        // Start with the first entry pre-filled and enqidx at 1
        Node(T *item)
            : deqidx{ 0 }
            , enqidx{ 1 }
            , next{ nullptr }
        {
            items[0].store(item, std::memory_order_relaxed);
            for (long i = 1; i < BUFFER_SIZE; i++)
            {
                items[i].store(nullptr, std::memory_order_relaxed);
            }
        }

        bool casNext(Node *cmp, Node *val) { return next.compare_exchange_strong(cmp, val); }
    };

    bool casTail(Node *cmp, Node *val) { return tail.compare_exchange_strong(cmp, val); }

    static T *takenPtr() { return (T *)TAKEN_PTR; }

    /**
     * Needed as enqueue needs protection from dequeue delete
     * when enqueue from 2 thread and dequeue from a thread at a same time. One enq thread created new node while deq finished and deleted
     * other enq thread's local node
     */
    HazardPointersManager<Node> hazardsManager;

    // Pointers to head and tail of the list
    alignas(2 * CACHE_LINE_SIZE) Node *head;
    alignas(2 * CACHE_LINE_SIZE) std::atomic<Node *> tail;

public:
    using HazardToken = HazardPointersManager<Node>::HazardPointer;

    FAAArrayMPSCQueue()
    {
        Node *sentinelNode = memNew<Node, Node *>(nullptr);
        sentinelNode->enqidx.store(0, std::memory_order_relaxed);
        head = sentinelNode;
        tail.store(sentinelNode, std::memory_order_relaxed);
    }

    ~FAAArrayMPSCQueue()
    {
        // Drain the queue
        while (dequeue() != nullptr)
        {}
        memDelete(head); // Delete the last node
    }

    inline HazardToken getHazardToken() { return { hazardsManager }; }

    void enqueue(T *item)
    {
        HazardToken token{ hazardsManager };
        enqueue(item, token);
    }
    void enqueue(T *item, HazardToken &hazardRecord)
    {
        if (item == nullptr)
            return;
        while (true)
        {
            Node *ltail = hazardRecord.record->setHazardPtr(tail);
            const int idx = ltail->enqidx.fetch_add(1, std::memory_order::acq_rel);
            if (idx > BUFFER_SIZE - 1) // This node is full
            {
                if (ltail != tail.load(std::memory_order::acquire))
                    continue;
                // It is okay CAS ensures we are fine
                Node *lnext = ltail->next.load(std::memory_order::relaxed);
                if (lnext == nullptr)
                {
                    // Try acquire existing deleting node if any present
                    Node *newNode = hazardsManager.dequeueDelete();
                    newNode = newNode ? newNode : (Node *)(CoPaTMemAlloc::memAlloc(sizeof(Node), alignof(Node)));
                    newNode = new (newNode) Node(item);
                    if (ltail->casNext(nullptr, newNode))
                    {
                        casTail(ltail, newNode);
                        hazardRecord.record->reset();
                        return;
                    }
                    memDelete(newNode);
                }
                else
                {
                    casTail(ltail, lnext);
                }
                continue;
            }
            T *itemnull = nullptr;
            if (ltail->items[idx].compare_exchange_strong(itemnull, item))
            {
                hazardRecord.record->reset();
                return;
            }
        }
    }

    T *dequeue()
    {
        while (true)
        {
            Node *lhead = head;
            // Do not want to delete head node if it is same as tail
            if (lhead->deqidx >= lhead->enqidx.load(std::memory_order::acquire) && lhead->next.load(std::memory_order::acquire) == nullptr)
            {
                break;
            }

            const int idx = lhead->deqidx++;
            if (idx > BUFFER_SIZE - 1) // This node has been drained, check if there is another one
            {
                Node *lnext = lhead->next.load(std::memory_order::acquire);
                if (lnext == nullptr) // No more nodes in the queue
                {
                    break;
                }
                head = lnext;
                hazardsManager.enqueueDelete(lhead);
                continue;
            }
            T *item = lhead->items[idx].exchange(takenPtr(), std::memory_order::relaxed);
            if (item == nullptr)
            {
                continue;
            }
            return item;
        }
        return nullptr;
    }
};

} // namespace copat

#endif /* _FAA_ARRAY_QUEUE_HP_H_ */
