/**
 * @file iomux.h
 *
 * @brief I/O multiplexer
 *
 */

#ifndef __IOMUX_H__
#define __IOMUX_H__

#ifdef __cplusplus
extern "C" {
#endif

#define IOMUX_DEFAULT_TIMEOUT 1

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
iomux_timeout_id_t iomux_set_timeout(iomux_t *iomux, int fd, struct timeval *timeout);

/**
 * @brief Register timed callback.
 * @param iomux The iomux handle
 * @param timeout The timeout to schedule
 * @param cb The callback to call when the timeout expires
 * @param priv A private context which will be passed to the callback
 * @returns The timeout id  on success; 0 otherwise.
 */
iomux_timeout_id_t iomux_schedule(iomux_t *iomux, struct timeval *timeout, iomux_cb_t cb, void *priv);

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
iomux_timeout_id_t iomux_reschedule(iomux_t *iomux, iomux_timeout_id_t id, struct timeval *timeout, iomux_cb_t cb, void *priv);

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
 *        at the end of each runcycle
 * @param iomux A valid iomux handler
 * @param cb The callback
 * @param priv A pointer which will be passed to the callback
 * @note iomux_loop() will run the mux (calling iomux_run()) with the
 *       provided timeout. The callback will be called when iomux_run() returns,
 *       just before checking for the leave condition and going ahead calling
 *       iomux_run() again
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
 * @brief Take over the runloop and handle timeouthandlers while running the mux.
 * @param iomux A valid iomux handler
 * @param timeout The maximum amount of time that iomux_loop can spend waiting for
 *                activity before checking for the end-of-loop and the hangup conditions
 * @note  If there is activity on a monitored filedescriptor or some timer has fired,
 *         the end-of-loop and hangup conditions might be checked before the timeout expires.
 */
void iomux_loop(iomux_t *iomux, struct timeval *timeout);

/**
 * @brief Stop a running mux and return control back to the iomux_loop() caller
 * @param iomux A valid iomux handler
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
 * @brief Checks if there is any managed filedescriptor in the iomux instance
 * @param iomux A valid iomux handler
 * @returns TRUE if success; FALSE otherwise
 */
int iomux_isempty(iomux_t *iomux);

#ifdef __cplusplus
}
#endif

#endif
