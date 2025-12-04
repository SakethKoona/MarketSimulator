use crate::types::PriceLevel;

const MAX_HEIGHT: usize = 24;

struct SkipListNode<T> {
    value: T,
    next: [Option<Box<SkipListNode<T>>>; MAX_HEIGHT],
    height: usize,
}

struct SkipList<T: Ord> {
    pub head: Box<SkipListNode<T>>,
    pub max_height: usize,

    // Private Variables
    length: usize,
}

impl<T: Ord> SkipList<T> {
    pub const MAX_HEIGHT: usize = 32;

    pub fn new(&self, head: Box<SkipListNode<T>>) -> Self {
        return SkipList::<T> {
            head: head,
            max_height: Self::MAX_HEIGHT,
            length: 0,
        };
    }

    pub fn search(&self, node: T) -> Option<&SkipListNode<T>> {
        // First, we get the head node
        let mut current = &*self.head;

        // Starts at the head node, at the topmost level, then keeps going down until we are at the
        // breaking point between the target and before, or as close as possible without being
        // greater than it.
        for level in (0..self.max_height).rev() {
            while let Some(ref next_node) = current.next[level] {
                if next_node.value == node {
                    return Some(next_node);
                } else if next_node.value < node {
                    current = next_node;
                } else {
                    break;
                }
            }
        }

        // The next part, now that we got as close as possible descending the levels on level 0 to
        // the node, we just do a regular linked list traversal to find the target or return None.
        while let Some(ref next_node) = current.next[0] {
            if next_node.value == node {
                return Some(next_node);
            } else {
                current = next_node;
            }
        }

        None
    }

    pub fn len(&mut self) -> usize {
        return self.length;
    }

    pub fn insert(&self, node: T) {
        todo!("Implement the insert method for the skiplist");
    }

    pub fn delete(&self, node: T) {
        todo!("Implement the delet method for the skiplist");
    }
}

impl SkipList<PriceLevel> {
    //! This function searches by the specific price level, and is only relevant for the PriceLevel
    //! version of this generic, it doesn't apply to all generic parameters T.
    pub fn search_by_price(&self, price: u32) -> Option<PriceLevel> {
        // First, we start at the head, which will have pointers to all levels up to max_height

        None
    }
}
