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
    pub probability: f32,
    // Private Variables
    length: usize,
}

impl<T: Ord> SkipList<T> {
    pub const MAX_HEIGHT: usize = 32;

    pub fn new(&self, head: Box<SkipListNode<T>>, probability: f32) -> Self {
        return SkipList::<T> {
            head: head,
            max_height: Self::MAX_HEIGHT,
            length: 0,
            probability: probability,
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
            } else if next_node.value > node {
                return None;
            } else {
                current = next_node;
            }
        }

        None
    }

    pub fn len(&mut self) -> usize {
        return self.length;
    }

    fn _create_new_node_helper(value: T) -> SkipListNode<T> {

    }

    pub fn insert(&self, target: T) {
        // First step, we basically go through the list and simulate a search to figure out where
        // that node would be if it were already inserted in level 0.
        // We need to keep track of the traversal path however, how are we going to do that?
        // Basically, we want the method to know exactly where to go when it actually creates that
        // node and explores

        // For each level, we basically had a target, which represented where to stop at that level
        //

        let mut current = &*self.head;
        let mut stopping_points: [&SkipListNode<T>; MAX_HEIGHT] = [&*self.head; MAX_HEIGHT]; // Initialize

        for level in (0..self.max_height).rev() {
            while let Some(ref next_node) = current.next[level] {
                if next_node.value >= target {
                    break; // break here
                } else {
                    current = next_node;
                }
            }

            stopping_points[level] = current; // record last stopping point after traversing level
        }

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
