#include "bh.h"
#include "bsd_queue.h"
#include <stdlib.h>
#include <string.h>

typedef struct _binomial_tree_node_s {
    uint64_t key;
    void *value;
    size_t vlen;
    struct _binomial_tree_node_s *parent;
    struct _binomial_tree_node_s **children;
    int num_children;
    bh_t *bh;
    TAILQ_ENTRY(_binomial_tree_node_s) next;
} binomial_tree_node_t;

struct _bh_s {
    TAILQ_HEAD(, _binomial_tree_node_s) trees;
    binomial_tree_node_t *head;
    int count;
    bh_free_value_callback_t free_value_cb;
};

#define UPDATE_HEAD(_bh) { \
    _bh->head = NULL;\
    _bh->head = bh_get_minimum(_bh, NULL); \
}

static int
binomial_tree_node_add(binomial_tree_node_t *node,
                       binomial_tree_node_t *child)
{
    binomial_tree_node_t **children = realloc(node->children, sizeof(binomial_tree_node_t *) * (node->num_children + 1));
    if (!children)
        return -1;

    node->children = children;

    node->children[node->num_children++] = child;
    if (child->parent) {
        // TODO - remove the node
    }
    child->parent = node;
    return 0;
}

static int
binomial_tree_node_find_min_child(binomial_tree_node_t *node)
{
    if (!node->num_children)
        return -1;

    int min_child_index = 0;
    int i;
    for (i = 0; i < node->num_children; i++) {
        binomial_tree_node_t *cur = node->children[i];
        binomial_tree_node_t *min = node->children[min_child_index];
        if (cur->key <= min->key)
            min_child_index = i;
    }
    return min_child_index;
}

static int
binomial_tree_node_find_max_child(binomial_tree_node_t *node)
{
    if (!node->num_children)
        return -1;

    int max_child_index = 0;
    int i;
    for (i = 0; i < node->num_children; i++) {
        binomial_tree_node_t *cur = node->children[i];
        binomial_tree_node_t *max = node->children[max_child_index];
        if (cur->key > max->key)
            max_child_index = i;
    }
    return max_child_index;
}

static void
binomial_tree_node_increase_key(binomial_tree_node_t *node, int incr)
{

    if (incr == 0)
        return;

    node->key += incr;

    binomial_tree_node_t *parent = node->parent;

    while (parent && parent->key < node->key)
    {
        binomial_tree_node_t tmp;
        tmp.key = parent->key;
        tmp.value = parent->value;
        tmp.vlen = parent->vlen;

        parent->key = node->key;
        parent->value = node->value;
        parent->vlen = node->vlen;

        node->key = tmp.key;
        node->value = tmp.value;
        node->vlen = tmp.vlen;

        binomial_tree_node_t *op = parent;
        parent = parent->parent; 
        node = op;
    }
}

static void
binomial_tree_node_destroy(binomial_tree_node_t *node, bh_free_value_callback_t free_value_cb)
{
    int i;
    binomial_tree_node_t *new_parent = NULL;

    if (node->parent) {
        new_parent = node->parent;
        int node_index = -1;
        for (i = 0; i < new_parent->num_children; i++) {
            if (new_parent->children[i] == node) {
                node_index = i;
                break;
            }
        }
        if (new_parent->num_children && node_index >= 0) {
            int to_copy = new_parent->num_children - (node_index + 1);
            if (to_copy) {
                memcpy(&new_parent->children[node_index],
                       &new_parent->children[node_index+1],
                       sizeof(binomial_tree_node_t *) * to_copy);
            } else {
                // TODO - Error messages
                // (something is badly corrupted if we are here)
            }
            new_parent->num_children--;
        }
    } else {
        if (node->num_children) {
            int child_index = binomial_tree_node_find_min_child(node);

            if (child_index >= 0) {
                new_parent = node->children[child_index];
                if (child_index < node->num_children - 1) {
                    memmove(&node->children[child_index],
                            &node->children[child_index + 1],
                            sizeof(binomial_tree_node_t *) * (node->num_children - (child_index + 1)));
                }
                node->num_children--;
                new_parent->parent = NULL;
                TAILQ_INSERT_AFTER(&node->bh->trees, node, new_parent, next);
            }
        }
        TAILQ_REMOVE(&node->bh->trees, node, next);
    }

    for (i = 0; i < node->num_children; i++) {
        if (new_parent) {
            if (binomial_tree_node_add(new_parent, node->children[i]) != 0) {
                // TODO - Error Messages, the tree is corrupted (OOM occurred)
            }
        } else {
            node->children[i]->parent = NULL;
        }
    }

    node->bh->count--;

    if (free_value_cb)
        free_value_cb(node->value);

    if (node->children)
        free(node->children);

    free(node);
}

bh_t *
bh_create(bh_free_value_callback_t free_value_cb)
{
    bh_t *bh = calloc(1, sizeof(bh_t));
    if (!bh)
        return NULL;
    bh->free_value_cb = free_value_cb;
    TAILQ_INIT(&bh->trees);
    return bh;
}

static int
binomial_tree_clear_cb(bh_t *bh, uint64_t key, void *value, size_t vlen, void *priv)
{
    return -1;
}

void
bh_destroy(bh_t *bh)
{
    bh_foreach(bh, binomial_tree_clear_cb, NULL);
    free(bh);
}

static int binomial_tree_merge(binomial_tree_node_t *node1, binomial_tree_node_t *node2)
{
    binomial_tree_node_t **children = realloc(node1->children, sizeof(binomial_tree_node_t *) * (node1->num_children + 1));
    if (!children)
        return -1;
    node1->children = children;
    node1->children[node1->num_children++] = node2;
    node2->parent = node1;
    return 0;
}

static binomial_tree_node_t *
_bh_maxmin(bh_t *bh, uint32_t *index, int maxmin)
{
    binomial_tree_node_t *node = NULL;
    binomial_tree_node_t *curtree = NULL;
    int idx = 0;
    TAILQ_FOREACH(curtree, &bh->trees, next) {
        if (!node) {
            node = curtree;
            if (index)
                *index = idx;
            idx++;
            continue;
        }
        int is_bigger = (curtree->key >= node->key);
        if ((maxmin == 0 && is_bigger) || (maxmin != 0 && !is_bigger))
        {
            node = curtree;
            if (index)
                *index = idx;
        }
        idx++;
    }

    return node;
}

static binomial_tree_node_t *
bh_get_minimum(bh_t *bh, uint32_t *minidx)
{
    if (bh->head)
        return bh->head;

    binomial_tree_node_t *minroot = _bh_maxmin(bh, minidx, 1);

    return minroot;
}

static binomial_tree_node_t *
bh_get_maximum(bh_t *bh, uint32_t *maxidx)
{
    binomial_tree_node_t *maxroot = _bh_maxmin(bh, maxidx, 0);

    if (!maxroot)
        return NULL;

    if (maxroot->num_children) {
        while (maxroot->num_children) {
            int max_child_index = binomial_tree_node_find_max_child(maxroot);
            maxroot = maxroot->children[max_child_index];
        }
    }
    return maxroot; 
}


int
bh_insert(bh_t *bh, uint64_t key, void *value, size_t vlen)
{
    binomial_tree_node_t *node = calloc(1, sizeof(binomial_tree_node_t));
    if (!node)
        return -1;

    node->bh = bh;
    node->key = key;
    node->value = value;
    node->vlen = vlen;
    int order = 0;
    binomial_tree_node_t *tree = TAILQ_FIRST(&bh->trees);
    if (tree)
        TAILQ_REMOVE(&bh->trees, tree, next);
    while (tree && tree->num_children == order) {
        if (node->key <= tree->key) {
            if (binomial_tree_merge(node, tree) != 0)
                return -1;
        } else {
            if (binomial_tree_merge(tree, node) != 0)
                return -1;
            node = tree;
        }
        order++;
        tree = TAILQ_FIRST(&bh->trees);
        if (tree)
            TAILQ_REMOVE(&bh->trees, tree, next);
    }
    if (tree)
        TAILQ_INSERT_HEAD(&bh->trees, tree, next);
    TAILQ_INSERT_HEAD(&bh->trees, node, next);

    bh->count++;

    if (!bh->head)
        bh->head = node;
    else
        UPDATE_HEAD(bh);

    return 0;
}

int
bh_delete(bh_t *bh, uint64_t key, void **value, size_t *vlen)
{
    binomial_tree_node_t *tree = NULL;
    binomial_tree_node_t *curtree = NULL;
    TAILQ_FOREACH(curtree, &bh->trees, next) {
        if (curtree->key <= key) {
            if (tree) {
                if (tree->key <= curtree->key) {
                    tree = curtree;
                }
            } else {
                tree = curtree;
            }
        }
    }

    binomial_tree_node_t *to_delete = tree;
    while(to_delete && to_delete->key != key)
    {
        binomial_tree_node_t *next_tree = NULL;
        int i;
        for (i = 0; i < to_delete->num_children; i++) {
            binomial_tree_node_t *child = to_delete->children[i];
            
            if (child->key <= key) {
                if (next_tree) {
                    if (next_tree->key <= child->key) {
                        next_tree = child;
                    }
                } else {
                    next_tree = child;
                }
            }
        }
        if (next_tree) {
            to_delete = next_tree;
        } else {
            to_delete = NULL;
        }
    }

    if (to_delete) {
        if (value)
            *value = to_delete->value;
        if (vlen)
            *vlen = to_delete->vlen;
        binomial_tree_node_destroy(to_delete, value ? NULL : bh->free_value_cb);
        if (to_delete == bh->head)
            UPDATE_HEAD(bh);
        return 0;
    }
    return -1;
}

int
bh_maximum(bh_t *bh, uint64_t *key, void **value, size_t *vlen)
{
    binomial_tree_node_t *maxitem = bh_get_maximum(bh, NULL);

    if (!maxitem)
        return -1;

    if (key)
        *key = maxitem->key;
    if (value)
        *value = maxitem->value;
    if (vlen)
        *vlen = maxitem->vlen;

    return 0;
}

int
bh_minimum(bh_t *bh, uint64_t *key, void **value, size_t *vlen)
{
    binomial_tree_node_t *minitem = bh_get_minimum(bh, NULL);;

    if (!minitem)
        return -1;

    if (key)
        *key = minitem->key;
    if (value)
        *value = minitem->value;
    if (vlen)
        *vlen = minitem->vlen;
    return 0;
}

int
bh_delete_minimum(bh_t *bh, void **value, size_t *vlen)
{
    uint32_t minidx = 0;
    binomial_tree_node_t *minitem = bh_get_minimum(bh, &minidx);

    if (!minitem)
        return -1;

    if (value)
        *value = minitem->value;
    if (vlen)
        *vlen = minitem->vlen;

    binomial_tree_node_destroy(minitem, value ? NULL : bh->free_value_cb);

    if (bh->head == minitem)
        UPDATE_HEAD(bh);

    return 0;
}

int
bh_delete_maximum(bh_t *bh, void **value, size_t *vlen)
{
    uint32_t maxidx = 0;
    binomial_tree_node_t *maxitem = bh_get_maximum(bh, &maxidx);

    if (!maxitem)
        return -1;

    if (value)
        *value = maxitem->value;
    if (vlen)
        *vlen = maxitem->vlen;

    binomial_tree_node_destroy(maxitem, value ? NULL : bh->free_value_cb);

    if (bh->head == maxitem)
        UPDATE_HEAD(bh);

    return 0;
}


uint32_t
bh_count(bh_t *bh)
{
    return bh->count;
}

// merge two heaps in a single iteration
bh_t *bh_merge(bh_t *bh1, bh_t *bh2)
{

    bh_t *merged_heap = bh_create(bh1->free_value_cb);

    binomial_tree_node_t *node1 = TAILQ_FIRST(&bh1->trees);
    if (node1)
        TAILQ_REMOVE(&bh1->trees, node1, next);
    binomial_tree_node_t *node2 = TAILQ_FIRST(&bh2->trees);
    if (node2)
        TAILQ_REMOVE(&bh2->trees, node2, next);
    binomial_tree_node_t *carry = NULL;

    while (node1 || node2 || carry) {

        if (carry) {
            // if we have a carry (merged tree from previous iteration)
            // lets check if either node1 or node2 is of the same order and
            // in case let's merge it before comparing node1 with node2 so 
            // we get rid of the carry as soon as possible
            binomial_tree_node_t *node = NULL;
            if (node1 && node1->num_children == carry->num_children) {
                node = node1;
            } else if (node2 && node2->num_children  == carry->num_children) {
                node = node2;
            } else {
                if (!node1 && !node2) {
                    // if we have the carry but there is neither node1 nor node2
                    // we can just add the carry to the list and forget about it
                    TAILQ_INSERT_TAIL(&merged_heap->trees, carry, next);
                    carry = NULL;
                    continue;
                }

                // if either node1 or node2 is of an higher order than the carry,
                // let's swap it so that we will always compare the lower order trees
                // among the three (node1, node2 and carry)
                if (node1 && node1->num_children > carry->num_children) {
                    binomial_tree_node_t *tmp = node1;
                    node1 = carry;
                    carry = tmp;
                } else if (node2 && node2->num_children > carry->num_children) {
                    binomial_tree_node_t *tmp = node2;
                    node2 = carry;
                    carry = tmp;
                }
            }

            if (node) {
                if (node->key <= carry->key) {
                    if (binomial_tree_merge(node, carry) != 0)
                        return NULL;
                } else {
                    if (binomial_tree_merge(carry, node) != 0)
                        return NULL;
                    if (node == node1)
                        node1 = carry;
                    else
                        node2 = carry;
                }
                carry = NULL;
            }
        }

        // we have already taken care of the carry here
        // so now if either node1 or node2 is missing 
        // we can just add the other to the list and go ahead
        if (node1 && !node2) {
            TAILQ_INSERT_TAIL(&merged_heap->trees, node1, next);
            node1 = TAILQ_FIRST(&bh1->trees);
            if (node1)
                TAILQ_REMOVE(&bh1->trees, node1, next);
            continue;
        } else if (node2 && !node1) {
            TAILQ_INSERT_TAIL(&merged_heap->trees, node2, next);
            node2 = TAILQ_FIRST(&bh2->trees);
            if (node2)
                TAILQ_REMOVE(&bh2->trees, node2, next);
            continue;
        } else if (carry && !node1 && !node2) {
            // XXX - this case should have already been handled earlier
            //       (we have a carry but neither node1 nor node2)
            TAILQ_INSERT_TAIL(&merged_heap->trees, carry, next);
            carry = NULL;
            continue;
        }

        
        int order1 = node1->num_children;
        int order2 = node2->num_children;

        // compare node1 and node2 and if they are of different orders
        // let's add the lower one to the list and go ahead
        if (order1 < order2) {
            TAILQ_INSERT_TAIL(&merged_heap->trees, node1, next);
            node1 = TAILQ_FIRST(&bh1->trees);
            if (node1)
                TAILQ_REMOVE(&bh1->trees, node1, next);
            continue;
        } else if (order1 > order2) {
            TAILQ_INSERT_TAIL(&merged_heap->trees, node2, next);
            node2 = TAILQ_FIRST(&bh2->trees);
            if (node2)
                TAILQ_REMOVE(&bh2->trees, node2, next);
            continue;
        }

        // if we are here node1 and node2 have the same order so they
        // need to be merged
        if (node1->key <= node2->key) {
            if (binomial_tree_merge(node1, node2) != 0)
                return NULL;
            if (carry) {
                if (node1->key >= carry->key) {
                    if (binomial_tree_merge(node1, carry) != 0)
                        return NULL;
                    carry = node1;
                } else {
                    if (binomial_tree_merge(carry, node1) != 0)
                        return NULL;
                }
            } else {
                carry = node1;
            }
        } else {
            if (binomial_tree_merge(node2, node1) != 0)
                return NULL;
            if (carry) {
                if (node2->key <= carry->key) {
                    if (binomial_tree_merge(node2, carry) != 0)
                        return NULL;
                    carry = node2;
                } else {
                    if (binomial_tree_merge(carry, node2) != 0)
                        return NULL;
                }
            } else {
                carry = node2;
            }
        }

        // the two trees (node1 and node2) have been merged and put into carry,
        // so let's get the next two nodes (if any) and go ahead
        node1 = TAILQ_FIRST(&bh1->trees);
        if (node1)
            TAILQ_REMOVE(&bh1->trees, node1, next);
        node2 = TAILQ_FIRST(&bh2->trees);
        if (node2)
            TAILQ_REMOVE(&bh2->trees, node2, next);
    }

    merged_heap->count = bh1->count + bh2->count;

    return merged_heap;
}

void
bh_increase_maximum(bh_t *bh, int incr)
{
    binomial_tree_node_t *maxitem = bh_get_maximum(bh, NULL);

    if (!maxitem)
        return;

    binomial_tree_node_increase_key(maxitem, incr);
}

void
bh_decrease_maximum(bh_t *bh, int decr)
{
    binomial_tree_node_t *maxitem = bh_get_maximum(bh, NULL);

    if (!maxitem)
        return;

    binomial_tree_node_increase_key(maxitem, -decr);
}


void
bh_increase_minimum(bh_t *bh, int incr)
{
    binomial_tree_node_t *minitem = bh_get_minimum(bh, NULL);

    if (!minitem)
        return;

    binomial_tree_node_increase_key(minitem, incr);
}

void
bh_decrease_minimum(bh_t *bh, int decr)
{
    binomial_tree_node_t *minitem = bh_get_minimum(bh, NULL);

    if (!minitem)
        return;

    binomial_tree_node_increase_key(minitem, -decr);
}

static int
binomial_tree_walk(binomial_tree_node_t *node, bh_iterator_callback cb, void *priv)
{
    int proceed = 0;
    int remove = 0;
    int rc = cb(node->bh, node->key, node->value, node->vlen, priv);
    switch(rc) {
        case -2:
            proceed = 0;
            remove = 1;
            break;
        case -1:
            proceed = 1;
            remove = 1;
            break;
        case 0:
            proceed = 0;
            remove = 0;
            break;
        case 1:
            proceed = 1;
            remove = 0;
            break;
        default:
            // TODO - Warning messages? (the callback returned an invalid return code)
            break;
    }
    if (proceed) {
        int i;
        for (i = 0; i < node->num_children; i ++) {
            binomial_tree_node_t *child = node->children[i];
            proceed = binomial_tree_walk(child, cb, priv); 
            if (!proceed)
                break;
        }
    }
    if (remove) {
        binomial_tree_node_destroy(node, node->bh->free_value_cb);
    }
    return proceed;
}

void
bh_foreach(bh_t *bh, bh_iterator_callback cb, void *priv)
{
    binomial_tree_node_t *curtree = NULL;
    binomial_tree_node_t *tmp;
    TAILQ_FOREACH_SAFE(curtree, &bh->trees, next, tmp) {
        if (binomial_tree_walk(curtree, cb, priv) == 0)
            break;
    }
}
