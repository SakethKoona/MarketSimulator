#include "arena.hpp"
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <unordered_map>

#define MAX_HEIGHT 16

template <typename Key, typename Value> struct SkipListNode {
    Key key;
    Value value;
    SkipListNode *forward[MAX_HEIGHT];
    uint16_t height;
    SkipListNode *free_next_;

    // Creating the constructor
    SkipListNode(Key k, Value v, uint16_t h)
        : key(k), value(v), height(h), free_next_(nullptr) {
        for (int i = 0; i < MAX_HEIGHT; i++) {
            forward[i] = nullptr;
        }
    }

    SkipListNode<Key, Value> *Next(int level) {
        if (level > height)
            throw std::runtime_error(
                "Cannot go higher than this node's height");
        return forward[level];
    }

    // Destructor
    void Clear() {
        // Free all the forward pointers
        for (int i = 0; i < MAX_HEIGHT; i++) {
            forward[i] = nullptr;
        }
        free_next_ = nullptr;
    }
};

template <typename Key, typename Value> class SkipList {
  public:
    SkipList(float p)
        : p(p), length_(0), tail(nullptr), allocator_(1024 * 1024),
          pool_(allocator_) {
        this->head_ptr = pool_.allocate(Key{}, Value{}, MAX_HEIGHT);
    }

    ~SkipList() {
        auto *current = head_ptr->forward[0];
        while (current) {
            auto *next = current->forward[0];
            pool_.deallocate(current);
            current = next;
        }

        pool_.deallocate(head_ptr);
    }

    // SkipList(const SkipList&) = delete;
    // SkipList& operator=(const SkipList&) = delete;

    SkipListNode<Key, Value> *head_ptr;
    float p;

    // Methods
    SkipListNode<Key, Value> *search(Key key) {
        // First, we get the head head_ptr
        auto *current = head_ptr;

        // Loop from top left to bottom right to find the position
        for (int level = MAX_HEIGHT - 1; level >= 0; level--) {
            while (current->forward[level] != nullptr &&
                   current->forward[level]->key < key) {
                current = current->forward[level];
            }
        }

        // Next, it's just a regular linked list traversal to the end
        current = current->forward[0];
        if (current && current->key == key) {
            return current;
        }

        return nullptr;
    }

    SkipListNode<Key, Value> *insertOrGet(const Key &key) {
        // First, we look in the hashmap, if so, we just return that for O(1)
        auto it = nodeLookup_.find(key);
        if (it != nodeLookup_.end()) {
            return it->second;
        }

        auto *current = head_ptr;
        SkipListNode<Key, Value> *stopping_points[MAX_HEIGHT] = {nullptr};

        // First, we do a similar traversal like in search, keeping track of
        // where we stop at each level
        for (int level = MAX_HEIGHT - 1; level >= 0; level--) {
            while (current->forward[level] != nullptr &&
                   current->forward[level]->key < key) {
                current = current->forward[level];
            }
            stopping_points[level] = current;
        }

        int lvl = getRandomLevel();
        Value value{}; // Value is initially empty

        // Actually create the newNode
        auto *newNodePtr =
            pool_.allocate(key, value, static_cast<uint16_t>(lvl));

        // After we get the random level, we now run through the loop again
        // and insert it where it should be
        for (int i = 0; i <= lvl; i++) {
            newNodePtr->forward[i] = stopping_points[i]->forward[i];
            stopping_points[i]->forward[i] = newNodePtr;
        }

        nodeLookup_.emplace(key, newNodePtr);

        if (newNodePtr->forward[0] ==
            nullptr) { // This means that we are at the end, (the max value)
            tail = newNodePtr;
        }

        this->length_++;
        return newNodePtr;
    }

    int len() { return this->length_; }
    SkipListNode<Key, Value> *getMax() { return tail; }

    SkipListNode<Key, Value> *GetHead() const { return head_ptr->forward[0]; }

    bool delete_node(const Key &key) {
        auto *current = head_ptr;
        SkipListNode<Key, Value> *update[MAX_HEIGHT] = {nullptr};
        // First part is same as insert
        for (int level = MAX_HEIGHT - 1; level >= 0; level--) {
            while (current->forward[level] &&
                   current->forward[level]->key < key) {
                current = current->forward[level];
            }
            update[level] = current;
        }

        // Next, we just remove references at each level
        auto *target = update[0]->forward[0];
        if (target && target->key == key) {
            // Before we rewire, let's reassign the tail if needed
            if (target->forward[0] == nullptr) {
                tail = (len() == 1) ? nullptr : update[0];
            }

            for (int i = 0; i < MAX_HEIGHT; i++) {
                if (update[i]->forward[i] == target) {
                    update[i]->forward[i] = target->forward[i];
                }
            }

            nodeLookup_.erase(key);
        } else { // Node to delete doesn't exist
            return false;
        }

        pool_.deallocate(target);
        length_--;
        return true;
    }

    void printList() const {
        // Regular traversal from level 0
        auto *current = head_ptr;
        while (current) {
            std::cout << "(" << current->key << ", " << current->value << ")"
                      << " -> ";
            current = current->forward[0];
        }

        std::cout << "END" << std::endl;
    }

  private:
    int length_;
    SkipListNode<Key, Value> *tail;
    ArenaAllocator allocator_;
    ArenaPool<SkipListNode<Key, Value>> pool_;
    std::unordered_map<Key, SkipListNode<Key, Value> *> nodeLookup_;

    int getRandomLevel() {
        int rand_level = 0;
        double random_variable;

        do {
            random_variable = static_cast<double>(std::rand()) / RAND_MAX;
            rand_level++;

        } while (random_variable > this->p && rand_level < MAX_HEIGHT);

        return rand_level - 1;
    }
};
