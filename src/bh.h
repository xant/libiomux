/**
 * @file bh.h
 *
 */

#ifndef IOMUX_BH_H
#define IOMUX_BH_H

#include <sys/types.h>
#include <stdint.h>

/**
 * @brief Opaque structure representing the heap
 */
typedef struct _bh_s bh_t;

typedef void (*bh_free_value_callback_t)(void *value);

/**
 * @brief Create a new binomial heap
 * @param free_value_cb If not null this callback will be used to release
 *                      the value stored in a specific node if the heap is being
 *                      cleared/destroyed while there are still nodes in it
 * @return               A valid and initialized binomial heap (empty)
 */
bh_t *bh_create(bh_free_value_callback_t free_value_cb);

/**
 * @brief Release all the resources used by a binomial heap
 * @param bh A valid pointer to an initialized bh_t structure
 */
void bh_destroy(bh_t *bh);

/**
 * @brief Insert a new value into the heap
 * @param bh A valid pointer to an initialized bh_t structure
 * @param key  The key of the node where to store the new value
 * @param value The new value to store
 * @param vlen  The size of the value
 * @return 0 if a new node has been inserted successfully;
 *         -1 otherwise
 */
int bh_insert(bh_t *bh, uint64_t key, void *value, size_t vlen);

/**
 * @brief Retrieve the minimum item in the heap
 * @param bh A valid pointer to an initialized bh_t structure
 * @param key   If not null will be set to point to the minimum key in the heap
 * @param value If not null will be set to point to the value for the minimum item
 * @param vlen  If not null will be set to point to the size of the value
 * @return 0 if the minimum item has been successfully found,\n
 *         -1 in case of errors
 */
int bh_minimum(bh_t *bh, uint64_t *key, void **value, size_t *vlen);

/**
 * @brief Retrieve the maximum item in the heap
 * @param bh A valid pointer to an initialized bh_t structure
 * @param key   If not null will be set to point to the maximum key in the heap
 * @param value If not null will be set to point to the value for the maximum item
 * @param vlen  If not null will be set to point to the size of the value
 * @return 0 if the maximum item has been successfully found,\n
 *         -1 in case of errors
 */
int bh_maximum(bh_t *bh, uint64_t *key, void **value, size_t *vlen);

/**
 * @brief Delete the minimum item in the heap (and eventually retrieve its value) 
 * @param bh A valid pointer to an initialized bh_t structure
 * @param value If not null will be set to point to the value for the minimum item
 *              being removed
 * @param vlen  If not null will be set to point to the size of the value
 * @return 0 if the  minimum item has been found and removed successfully,\n
 *         -1 in case of errors
 */
int bh_delete_minimum(bh_t *bh, void **value, size_t *vlen);

/**
 * @brief Delete the maximum item in the heap (and eventually retrieve its value) 
 * @param bh A valid pointer to an initialized bh_t structure
 * @param value If not null will be set to point to the value for the maximum item
 *              being removed
 * @param vlen  If not null will be set to point to the size of the value
 * @return 0 if the  minimum item has been found and removed successfully,\n
 *         -1 in case of errors
 */
int bh_delete_maximum(bh_t *bh, void **value, size_t *vlen);


/**
 * @brief Delete at most one item matching a given key in the heap
 * @param bh A valid pointer to an initialized bh_t structure
 * @param key The key to match
 * @param value If not null will be set to point to the value for the maximum item
 *              being removed
 * @param vlen  If not null will be set to point to the size of the value
 * @return 0 if least one item has been found matching the given key
 *         and it has been removed successfully,\n -1 in case of errors
 */
int bh_delete(bh_t *bh, uint64_t key, void **value, size_t *vlen);

/**
 * @brief Increase the minimum key by an arbitrary amount
 * @param bh A valid pointer to an initialized bh_t structure
 * @param incr The amount to increase the minimum key by
 */
void bh_increase_minimum(bh_t *bh, int incr);

/**
 * @brief Increase the maximum key by an arbitrary amount
 * @param bh A valid pointer to an initialized bh_t structure
 * @param incr The amount to increase the maximum key by
 */
void bh_increase_maximum(bh_t *bh, int incr);

/**
 * @brief Decrease the minimum key by an arbitrary amount
 * @param bh A valid pointer to an initialized bh_t structure
 * @param decr The amount to decrease the minimum key by
 */
void bh_decrease_minimum(bh_t *bh, int decr);

/**
 * @brief Decrease the maximum key by an arbitrary amount
 * @param bh A valid pointer to an initialized bh_t structure
 * @param decr The amount to decrease the maximum key by
 */
void bh_decrease_maximum(bh_t *bh, int decr);

/**
 * @brief Increase a given key by an arbitrary amount
 * @param bh A valid pointer to an initialized bh_t structure
 * @param key The key to increase
 * @param incr The amount to increase the key by
 */
void bh_increase_key(bh_t *bh, uint64_t key, int incr);

/**
 * @brief Decrease a given key by an arbitrary amount
 * @param key The key to increase
 * @param bh A valid pointer to an initialized bh_t structure
 * @param decr The amount to decrease the key by
 */
void bh_decrease_key(bh_t *bh, uint64_t key, int decr);

/**
 * @brief Merge two heaps
 * @param bh1 A valid pointer to an initialized bh_t structure
 * @param bh2 A valid pointer to an initialized bh_t structure
 * @return A newly created heap which will contain the union of the items
 *         stored in both the heaps (bh1 and bh2) provided as argument.
 *         The caller is responsible of disposing the new heap.
 * @note Both bh1 and bh2 will be empty once merged in the new returned heap.
 *       The caller is responsible of disposing both of them if not necessary
 *       anymore (otherwise further operations on the original heaps are still
 *       possible)
 * @note The two heaps MUST be configured to use the same operational mode
 *       for them to be merged. If the operational modes differ no merge 
 *       will be attempted and NULL will be returned
 */
bh_t *bh_merge(bh_t *bh1, bh_t *bh2);

/**
 * @brief Return the number of items in the heap
 * @param bh A valid pointer to an initialized bh_t structure
 * @return the actual number of items in the heap
 */
uint32_t bh_count(bh_t *bh);

typedef int (*bh_iterator_callback)(bh_t *bt, uint64_t key, void *value, size_t vlen, void *priv);
void bh_foreach(bh_t *bh, bh_iterator_callback cb, void *priv);

#endif
