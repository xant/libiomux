/**
 * @file iomux.h
 *
 * @brief I/O multiplexer
 *
 * @todo Handle timeouts using a priority queue
 *
 */

#ifndef __IOMUX_H__
#define __IOMUX_H__

#ifdef __cplusplus
extern "C" {
#endif

//! if set to true, the hangup callback (if any) will be called at the end of the current runcycle
extern int iomux_hangup;

typedef struct __iomux iomux_t;
typedef void (*iomux_cb_t)(iomux_t *iomux, void *priv);

typedef int iomux_timeout_id_t;

/**
 * @struct iomux_callbacks_t
 * @brief iomux callbacks structure
 */
typedef struct __iomux_callbacks {
    //! The callback called when there is new data on the monitored fd
    void (*mux_input)(iomux_t *iomux, int fd, void *data, int len, void *priv);
    //! If not NULL, it will be called when it's possible to write new data on fd 
    void (*mux_output)(iomux_t *iomux, int fd, void *priv);
    //! If not NULL, it will be called when a timeout on fd expires
    void (*mux_timeout)(iomux_t *iomux, int fd, void *priv);
    //! If not NULL, it will be called when EOF is reached and the fd can be closed
    void (*mux_eof)(iomux_t *iomux, int fd, void *priv);
    //! If not NULL and fd is a listening socket, it will be called when a new connection is accepted on fd 
    void (*mux_connection)(iomux_t *iomux, int fd, void *priv);
    //! A pointer to private data which will be passed to all the callbacks as last argument
    void *priv;
} iomux_callbacks_t;

/**
 * @brief Create a new iomux handler
 * @returns A valid iomux handler
 */
iomux_t *iomux_create(void);

void iomux_set_threadsafe(iomux_t *iomux, int threadsafe);

/**
 * @brief Add a filedescriptor to the mux
 * @param iomux A valid iomux handler
 * @param fd The fd to add
 * @param cbs The set of callbacks to use with fd
 * @returns TRUE on success; FALSE otherwise.
 */
int  iomux_add(iomux_t *iomux, int fd, iomux_callbacks_t *cbs);

/**
 * @brief Remove a filedescriptor from the mux
 * @param iomux A valid iomux handler
 * @param fd The fd to remove
 */
void iomux_remove(iomux_t *iomux, int fd);

/**
 * @brief Register a timeout on a connection.
 * @param iomux The iomux handle
 * @param fd The fd the timer relates to
 * @param timeout The timeout or NULL
 * @returns The timeout id  on success; 0 otherwise.
 * @note If timeout is NULL the timeout is disabled.
 * @note Needs to be reset after a timeout has fired.
 */
iomux_timeout_id_t iomux_set_timeout(iomux_t *iomux,
                                     int fd,
                                     struct timeval *timeout);

/**
 * @brief Register timed callback.
 * @param iomux The iomux handle
 * @param timeout The timeout to schedule
 * @param cb The callback to call when the timeout expires
 * @param priv A private context which will be passed to the callback
 * @returns The timeout id  on success; 0 otherwise.
 */
iomux_timeout_id_t iomux_schedule(iomux_t *iomux,
                                  struct timeval *timeout,
                                  iomux_cb_t cb,
                                  void *priv);

/**
 * @brief Reset the schedule time on a timed callback.
 * @param iomux The iomux handle
 * @param id The id of the timeout to reset
 * @param timeout The new timeout
 * @param cb The callback handle
 * @param priv A private context which will be passed to the callback
 * @returns the timeout id  on success; 0 otherwise.
 *
 * @note If the timed callback is not found it is added.
 */
iomux_timeout_id_t iomux_reschedule(iomux_t *iomux,
                                    iomux_timeout_id_t id,
                                    struct timeval *timeout,
                                    iomux_cb_t cb,
                                    void *priv);

/**
 * @brief Unregister a specific timeout callback.
 * @param iomux The iomux handle
 * @param id The timeout id
 * @returns TRUE on success; FALSE otherwise.
 */
int  iomux_unschedule(iomux_t *iomux, iomux_timeout_id_t id);

/**
 * @brief Unregister all timers for a given callback.
 * @param iomux The iomux handle
 * @param cb The callback handle
 * @param priv The context
 * @note Removes _all_ instances that match.
 * @returns The number of removed callbacks.
 */
int  iomux_unschedule_all(iomux_t *iomux, iomux_cb_t cb, void *priv);

/**
 * @brief Put a filedescriptor to listening state (aka: server connection)
 * @param iomux A valid iomux handler
 * @param fd The fd to put in listening state
 * @returns TRUE on success; FALSE otherwise.
 */
int  iomux_listen(iomux_t *iomux, int fd);

/**
 * @brief Register the callback which will be called by iomux_loop()
 *        at each runcycle before calling iomux_run()
 * @param iomux A valid iomux handler
 * @param cb The callback
 * @param priv A pointer which will be passed to the callback
 * @note iomux_loop() will run the mux (calling iomux_run()) with the
 *       provided timeout. The loop_next callback will be called when
 *       iomux_run() returns, just before checking for the leave condition
 *       and going ahead calling iomux_run() again
 */
void iomux_loop_next_cb(iomux_t *iomux, iomux_cb_t cb, void *priv);

/**
 * @brief Register the callback which will be called by iomux_loop()
 *        before returning, after the loop has been ended
 * @param iomux A valid iomux handler
 * @param cb The callback
 * @param priv A pointer which will be passed to the callback
 */
void iomux_loop_end_cb(iomux_t *iomux, iomux_cb_t cb, void *priv);

/**
 * @brief Register the callback which will be called by iomux_loop()
 *        at the end the runcycle if iomux_hangup is set to TRUE
 * @param iomux A valid iomux handler
 * @param cb The callback
 * @param priv A pointer which will be passed to the callback
 */
void iomux_hangup_cb(iomux_t *iomux, iomux_cb_t cb, void *priv);

/**
 * @brief Take over the runloop and handle timers while running the mux.
 * @param iomux A valid iomux handler
 * @param timeout The maximum amount of time that iomux_loop() can spend waiting
 *                for activity before checking for the end-of-loop and the
 *                hangup conditions
 * @note If there is activity on a monitored filedescriptor or some timer has
 *       fired, the end-of-loop and hangup conditions might be checked before
 *       the whole timeout has passed.
 * @note Before returning the end_loop callback (if anyw) will be called
 */
void iomux_loop(iomux_t *iomux, struct timeval *timeout);

/**
 * @brief Stop a running mux and return control back to the
 *        iomux_loop() caller
 * @param iomux A valid iomux handler
 * @note If an end_loop callback is registered, it will be called by
 *       iomux_loop() just before returning to the caller.
 */
void iomux_end_loop(iomux_t *iomux);

/**
 * @brief Trigger a runcycle on an iomux
 * @param iomux A valid iomux handler
 * @param timeout Return control to the caller if nothing
 *        happens in the mux within the specified timeout
 * @note The underlying implementation will use: 
 *       epoll_wait(), kevent() or select()
 *       depending on the flags used at compile time
 */
void iomux_run(iomux_t *iomux, struct timeval *timeout);

/**
 * @brief Write to an fd handled by the iomux
 * @param iomux A valid iomux handler
 * @param fd The fd we want to write to
 * @param buf The buffer to write
 * @param len The length of the buffer
 * @returns The number of written bytes
 */
int iomux_write(iomux_t *iomux, int fd, const void *buf, int len);

/**
 * @brief Close a file handled by the iomux
 * @param iomux A valid iomux handler
 * @param fd The fd to close
 */
void iomux_close(iomux_t *iomux, int fd);

/**
 * @brief Relase all resources used by an iomux
 * @param iomux A valid iomux handler
 */
void iomux_destroy(iomux_t *iomux);

/**
 * @brief Clear the iomux by removing all filedescriptors and timeouts
 * @param iomux A valid iomux handler
 */
void iomux_clear(iomux_t *iomux);

/**
 * @brief Checks if there is any managed filedescriptor in the iomux instance
 * @param iomux A valid iomux handler
 * @returns TRUE if success; FALSE otherwise
 */
int iomux_isempty(iomux_t *iomux);

/**
 * @brief Get the available write buffer size for the given fd
 * @param iomux A valid iomux handler
 * @param fd The filedescriptor for which to check the available write buffer
 *           size
 * @return The available size (in bytes), -1 if the filedescriptor is unknown
 *         to the mux
 */
int iomux_write_buffer(iomux_t *iomux, int fd);

/**
 * @brief Return the callbacks descriptor for the given fd
 * @param iomux A valid iomux handler
 * @param fd The filedescriptor for which to return the callbacks descriptor
 * @return A pointer to the iomux_callbacks_t structure holding the callbacks
 *         registered for the given fd
 * @note The caller can change the pointers unregistering existing callbacks
 *       or registering new ones.
 */
iomux_callbacks_t *iomux_callbacks(iomux_t *iomux, int fd);

#ifndef NO_PTHREAD
typedef struct __iomtee_s iomtee_t;

/**
 * @brief Open a new multi-tee
 * @param vfd If not null, the value of the tee filedescriptor will be stored
 *            at the memory pointed by vfd
 * @param num_fds The number of filedescriptors which will receive everything
 *                being written to vfd
 * @return A multi-tee handler
 */
iomtee_t *iomtee_open(int *vfd, int num_fds, ...);
/**
 * @brief Get the multi-tee filedescriptor
 *
 * The returned filedescriptor can be used for write operations which need
 * to be multiplexed to the filedescriptors registered at iomtee_open()
 *
 * @param tee A valid multi-tee handler
 * @return the multi-tee filedescriptor to use for write operations
 * @note The same filedescriptor is stored in the 'vfd' parameter in 
 *       iomtee_open()
 */
int iomtee_fd(iomtee_t *tee);
/**
 * @brief Close the multi-tee and dispose all resources
 * @param tee A valid multi-tee handler
 */
void iomtee_close(iomtee_t *tee);

void iomtee_add_fd(iomtee_t *tee, int fd);

void iomtee_remove_fd(iomtee_t *tee, int fd);

#endif

#ifdef __cplusplus
}
#endif

#endif
