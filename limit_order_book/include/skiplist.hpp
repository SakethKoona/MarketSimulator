#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>

#define MAX_HEIGHT 16

template <typename Key, typename Value> struct SkipListNode {
  Key key;
  Value value;
  SkipListNode *forward[MAX_HEIGHT];
  uint16_t height;

  // Creating the constructor
  SkipListNode(Key k, Value v, uint16_t h) : key(k), value(v), height(h) {
    for (int i = 0; i < MAX_HEIGHT; i++) {
      forward[i] = nullptr;
    }
  }
};

template <typename Key, typename Value> class SkipList {
public:
  SkipList(float p) : p(p) {
    this->head_ptr = new SkipListNode<Key, Value>(Key{}, Value{}, MAX_HEIGHT);
    this->length = 0;
  }

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
  

  //TODO: Also could possibly implement an insert method that takes in a key and a value and is a traidional insert method

  SkipListNode<Key, Value> *insertOrGet(const Key &key) {
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

    current = current->forward[0];
    if (current != nullptr && current->key == key) {
      return current; // If we found it, return it
    }

    int lvl = getRandomLevel();
    Value value{};

    // Actually create the newNode
    auto *newNode =
        new SkipListNode<Key, Value>(key, value, static_cast<uint16_t>(lvl));

    // After we get the random level, we now run through the loop again
    // and insert it where it should be
    for (int i = 0; i <= lvl; i++) {
      newNode->forward[i] = stopping_points[i]->forward[i];
      stopping_points[i]->forward[i] = newNode;
    }

    if (newNode->forward[0] == nullptr) { // This means that we are at the end, (the max value)
      tail = newNode;
    }

    this->length++;
    return newNode;
  }

  int len() { return this->length; }
  SkipListNode<Key, Value>* getMax() { return tail; }

  bool delete_node(const Key &key) {
    auto *current = head_ptr;
    SkipListNode<Key, Value> *update[MAX_HEIGHT] = {nullptr};
    // First part is same as insert
    for (int level = MAX_HEIGHT - 1; level >= 0; level--) {
      while (current->forward[level] && current->forward[level]->key < key) {
        current = current->forward[level];
      }
      update[level] = current;
    }

    // Next, we just remove references at each level
    auto *target = update[0]->forward[0];
    if (target && target->key == key) {
      // Before we rewire, let's reassign the tail if needed
      if (target->forward[0] == nullptr) {
        tail = update[0]; // The new tail becomes the next biggest one because
                          // the current biggest is getting deleted
      }

      for (int i = 0; i < MAX_HEIGHT; i++) {
        if (update[i]->forward[i] == target) {
          update[i]->forward[i] = target->forward[i];
        }
      }
    } else { // We don't return anything, cause the node to delete doesn't exist
      return false;
    }

    delete target;
    length--;
    return true;
  }

  void printList() {
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
  int length;
  SkipListNode<Key, Value> *tail;
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
