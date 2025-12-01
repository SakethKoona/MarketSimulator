use crate::types::PriceLevel;


struct SkipListNode<T> {
    value: T,
    next: Vec<Option<Box<SkipListNode<T>>>>,
}

struct SkipList<T: Ord> {
    pub head: Box<SkipListNode<T>>,
    pub max_height: usize,
}

impl<T: Ord> SkipList<T> {
    pub const MAX_HEIGHT: usize = 32;

    pub fn new(&self, head: Box<SkipListNode<T>>) -> Self {
        return SkipList::<T> {
            head: head,
            max_height: Self::MAX_HEIGHT,
        };
    }

    pub fn search(&self, node: T) {
        todo!("Implement the search traversal for skiplist");
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
    pub fn search_by_price(&self, price: u32) {
        
        // First, we start at the highest level
        let starting_node = &self.head.next[0].expect("[FATAL ERROR]: Head Node is empty");

        while starting_node.next.is_empty() == false {
            todo!();
        }
    }
}
