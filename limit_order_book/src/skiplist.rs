

struct SkipListNode<T> {
    value: T,
    next: Vec<Option<Box<SkipListNode<T>>>>,
}

struct SkipList<T> {
    pub head: Box<SkipListNode<T>>,
    pub max_height: usize,
}

impl<T> SkipList<T> {
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
}
