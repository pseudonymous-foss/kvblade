#ifndef __CLYDEINTERFACE_H
#define __CLYDEINTERFACE_H

/**
 * Create a new tree.
 * @param k the k-value, nodes are split when there are 2k 
 *          children elements in them.
 * @return the tid (tree identifier) of the tree. 0 if no tree 
 *         could be created
 */
extern u64 clydefscore_tree_create(u8 k);

/**
 * Remove a tree and any child elements. 
 * @param tid the tree identifier 
 * @return 0 on success, negative on errors, positive values as 
 *         status codes. 1 => no such tree
 */
extern int clydefscore_tree_remove(u64 tid);

/**
 * Create a new node containing the data identified by 'data'. 
 * @param tid the tree identifier 
 * @param len the length of the data in bytes 
 * @param data a pointer to the data itself 
 * @return 0 on success. Negative values indicate errors. 
 *         -ENOMEM in particular if out of memory.
 *         -ENOENT => no tree by 'tid'
 */
extern int clydefscore_node_insert(u64 tid, u64 *nid);

/**
 * @param tid the tree identifier
 * @param nid the node identifier
 * @return 0 on success. Negative values on errors. Positive 
 *         values as status codes. 1 => no such node
 */
extern int clydefscore_node_remove(u64 tid, u64 nid);

/**
 * Read a sequence of data from the node 
 * @param tid the tree identifier 
 * @param nid the node identifier 
 * @param offset the offset within the node 
 * @param len amount to read in bytes 
 * @param data a buffer guaranteed to be at least large enough 
 *         to hold the requested data
 * @return 0 on success. Negative values on errors: -ENOENT if 
 *         the node doesn't exist
 */
extern int clydefscore_node_read(u64 tid, u64 nid, u64 offset, u64 len, void *data);

/** 
 * Write a sequence of bytes to the node. 
 * @param tid the identifier of the tree containing the node 
 * @param nid the node identifier 
 * @param offset the offset within the node from which to begin writing. In bytes. 
 * @param len the length of the buffer (in bytes). 
 * @param data a buffer containing the data to write. 
 * @return 0 on success. Negative values on errors. -ENOENT if 
 *         the node doesn't exist, -ENOMEM if space allocation
 *         for the data failed. 
 */
extern int clydefscore_node_write(u64 tid, u64 nid, u64 offset, u64 len, void *data);
#endif //__CLYDEINTERFACE_H
