#include <cstdlib>
#include <ctime>

#define MAX_HEIGHT 16

template <typename Key, typename Value> struct SkipListNode {
  Key key;
  Value value;
  SkipListNode *forward[MAX_HEIGHT];

  // Creating the constructor
  SkipListNode(Key k, Value v) : key(k), value(v) {
    for (int i = 0; i < MAX_HEIGHT; i++) {
      forward[MAX_HEIGHT] = nullptr;
    }
  }
};

template <typename Key, typename Value> class SkipList {
public:
  SkipList(float p) : p(p) {
    this->head_ptr = new SkipListNode<Key, Value>(Key{}, Value{});
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
      delete current;
      return current;
    }

    return nullptr;
  }

  bool insert(const Key &key, const Value &value) {
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

    // Actually create the newNode
    auto *newNode = new SkipListNode<Key, Value>(key, value);

    // After we get the random level, we now run through the loop again
    // and insert it where it should be
    for (int i = 0; i <= lvl; i++) {
      newNode->forward[i] = stopping_points[i]->forward[i];
      stopping_points[i]->forward[i] = newNode;
    }

    delete current;
    delete newNode;

    this->length++;
    return true;
  }

  int len() { return this->length; }

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

  }

private:
  int length;
  int getRandomLevel() {
    int rand_level = 0;
    double random_variable;

    do {
      random_variable = static_cast<double>(std::rand()) / RAND_MAX;
      rand_level++;

    } while (random_variable > this->p && rand_level < MAX_HEIGHT);

    delete &random_variable;
    return rand_level - 1;
  }
};
