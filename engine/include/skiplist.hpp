#pragma once

#include "arena.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <unordered_map>

#define MAX_HEIGHT 16

// A skiplist node sized to its own height: a node of height h carries h+1
// forward pointers, not MAX_HEIGHT. Nodes are allocated from the list's
// arena and recycled through per-height free lists.
template <typename Key, typename Value> struct SkipListNode {
    Key key;
    Value value;
    uint16_t height;
    SkipListNode *free_next_;
    SkipListNode *forward[1]; // actually height+1 entries; see node_bytes()

    SkipListNode(Key k, Value v, uint16_t h)
        : key(k), value(v), height(h), free_next_(nullptr) {
        for (int i = 0; i <= h; i++)
            forward[i] = nullptr;
    }

    SkipListNode<Key, Value> *Next(int level) {
        if (level > height)
            throw std::runtime_error(
                "Cannot go higher than this node's height");
        return forward[level];
    }

    static constexpr std::size_t node_bytes(int h) {
        return offsetof(SkipListNode, forward) +
               static_cast<std::size_t>(h + 1) * sizeof(SkipListNode *);
    }
};

template <typename Key, typename Value> class SkipList {
    using Node = SkipListNode<Key, Value>;

  public:
    SkipList(float p, std::size_t arena_block = 1 << 20)
        : p(p), length_(0), tail(nullptr), allocator_(arena_block) {
        for (auto &f : free_)
            f = nullptr;
        this->head_ptr = allocNode(Key{}, Value{}, MAX_HEIGHT - 1);
    }

    ~SkipList() {
        // Destroy values; the arena releases the memory itself.
        Node *current = head_ptr;
        while (current) {
            Node *next = current->forward[0];
            current->~Node();
            current = next;
        }
    }

    SkipList(const SkipList &) = delete;
    SkipList &operator=(const SkipList &) = delete;

    Node *head_ptr;
    float p;

    Node *search(Key key) {
        auto *current = head_ptr;
        for (int level = MAX_HEIGHT - 1; level >= 0; level--) {
            while (current->forward[level] != nullptr &&
                   current->forward[level]->key < key) {
                current = current->forward[level];
            }
        }
        current = current->forward[0];
        if (current && current->key == key) {
            return current;
        }
        return nullptr;
    }

    Node *insertOrGet(const Key &key) {
        auto it = nodeLookup_.find(key);
        if (it != nodeLookup_.end()) {
            return it->second;
        }

        auto *current = head_ptr;
        Node *stopping_points[MAX_HEIGHT] = {nullptr};

        for (int level = MAX_HEIGHT - 1; level >= 0; level--) {
            while (current->forward[level] != nullptr &&
                   current->forward[level]->key < key) {
                current = current->forward[level];
            }
            stopping_points[level] = current;
        }

        int lvl = getRandomLevel();
        Node *newNodePtr = allocNode(key, Value{}, static_cast<uint16_t>(lvl));

        for (int i = 0; i <= lvl; i++) {
            newNodePtr->forward[i] = stopping_points[i]->forward[i];
            stopping_points[i]->forward[i] = newNodePtr;
        }

        nodeLookup_.emplace(key, newNodePtr);

        if (newNodePtr->forward[0] == nullptr) {
            tail = newNodePtr;
        }

        this->length_++;
        return newNodePtr;
    }

    int len() const { return this->length_; }
    Node *getMax() { return tail; }

    Node *GetHead() const { return head_ptr->forward[0]; }

    bool delete_node(const Key &key) {
        auto *current = head_ptr;
        Node *update[MAX_HEIGHT] = {nullptr};
        for (int level = MAX_HEIGHT - 1; level >= 0; level--) {
            while (current->forward[level] &&
                   current->forward[level]->key < key) {
                current = current->forward[level];
            }
            update[level] = current;
        }

        Node *target = update[0]->forward[0];
        if (!target || target->key != key) {
            return false;
        }

        if (target->forward[0] == nullptr) {
            tail = (len() == 1) ? nullptr : update[0];
        }

        // update[i] was reached at level i, so its height is >= i
        for (int i = 0; i <= target->height; i++) {
            if (update[i]->forward[i] == target) {
                update[i]->forward[i] = target->forward[i];
            }
        }

        nodeLookup_.erase(key);
        freeNode(target);
        length_--;
        return true;
    }

    void printList() const {
        auto *current = head_ptr;
        while (current) {
            std::cout << "(" << current->key << ", " << current->value << ")"
                      << " -> ";
            current = current->forward[0];
        }
        std::cout << "END" << std::endl;
    }

    std::size_t arenaBlocks() const { return allocator_.blocks(); }

  private:
    int length_;
    Node *tail;
    ArenaAllocator allocator_;
    Node *free_[MAX_HEIGHT]; // free list per height
    std::unordered_map<Key, Node *> nodeLookup_;

    Node *allocNode(const Key &k, const Value &v, uint16_t h) {
        void *mem;
        if (free_[h]) {
            Node *n = free_[h];
            free_[h] = n->free_next_;
            mem = n;
        } else {
            mem = allocator_.allocate(Node::node_bytes(h), alignof(Node));
        }
        return new (mem) Node(k, v, h);
    }

    void freeNode(Node *n) {
        uint16_t h = n->height;
        n->~Node();
        n->free_next_ = free_[h];
        free_[h] = n;
    }

    // xorshift64: fast, no global state, deterministic per list
    uint64_t rng_state_ = 0x9E3779B97F4A7C15ULL;

    uint64_t nextRandom() {
        uint64_t x = rng_state_;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        rng_state_ = x;
        return x;
    }

    int getRandomLevel() {
        const uint64_t threshold = static_cast<uint64_t>(p * 4294967296.0);
        int level = 0;
        while (level < MAX_HEIGHT - 1 && (nextRandom() >> 32) < threshold)
            level++;
        return level;
    }
};
