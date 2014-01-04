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

//! @brief iomux callbacks structure
typedef struct __iomux_callbacks {
    void (*mux_input)(iomux_t *iomux, int fd, void *data, int len, void *priv);
    void (*mux_output)(iomux_t *iomux, int fd, void *priv);
    void (*mux_timeout)(iomux_t *iomux, int fd, void *priv);
    void (*mux_eof)(iomux_t *iomux, int fd, void *priv);
    void (*mux_connection)(iomux_t *iomux, int fd, void *priv);
    void *priv;
} iomux_callbacks_t;

/**
 * @brief Create a new iomux handler
 * @returns a valid iomux handler
 */
iomux_t *iomux_create(void);

/**
 * @brief Add a filedescriptor to the mux
 * @param iomux a valid iomux handler
 * @param fd fd to add
 * @param cbs set of callbacks to use with fd
 * @returns TRUE on success; FALSE otherwise.
 */
int  iomux_add(iomux_t *iomux, int fd, iomux_callbacks_t *cbs);

/**
 * @brief Remove a filedescriptor from the mux
 * @param iomux a valid iomux handler
 * @param fd fd to remove
 */
void iomux_remove(iomux_t *iomux, int fd);

/**
 * @brief Register a timeout on a connection.
 * @param iomux iomux handle
 * @param fd fd
 * @param tv timeout or NULL
 * @returns the timeout id  on success; 0 otherwise.
 * @note If tv is NULL the timeout is disabled.
 * @note Needs to be reset after a timeout has fired.
 */
iomux_timeout_id_t iomux_set_timeout(iomux_t *iomux, int fd, struct timeval *timeout);

/**
 * @brief Register timed callback.
 * @param iomux iomux handle
 * @param tv timeout
 * @param cb callback handle
 * @param priv context
 * @returns the timeout id  on success; 0 otherwise.
 */
iomux_timeout_id_t iomux_schedule(iomux_t *iomux, struct timeval *timeout, iomux_cb_t cb, void *priv);

/**
 * @brief Reset the schedule time on a timed callback.
 * @param iomux iomux handle
 * @param tv new timeout
 * @param cb callback handle
 * @param priv context
 * @returns the timeout id  on success; 0 otherwise.
 *
 * @note If the timed callback is not found it is added.
 */
iomux_timeout_id_t iomux_reschedule(iomux_t *iomux, iomux_timeout_id_t id, struct timeval *timeout, iomux_cb_t cb, void *priv);

/**
 * @brief Unregister a specific timeout callback.
 * @param iomux iomux handle
 * @param id the timeout id
 * @returns TRUE on success; FALSE otherwise.
 */
int  iomux_unschedule(iomux_t *iomux, iomux_timeout_id_t id);

/**
 * @brief Unregister all timers for a given callback.
 * @param iomux iomux handle
 * @param cb callback handle
 * @param priv context
 * @note Removes _all_ instances that match.
 * @returns number of removed callbacks.
 */
int  iomux_unschedule_all(iomux_t *iomux, iomux_cb_t cb, void *priv);

/**
 * @brief put and fd to listening state (aka: server connection)
 * @param iomux a valid iomux handler
 * @param fd the fd to put in listening state
 * @returns TRUE on success; FALSE otherwise.
 */
int  iomux_listen(iomux_t *iomux, int fd);

/**
 * @brief register the callback which will be called by iomux_loop()
 *        at the end of each runcycle
 * @param iomux a valid iomux handler
 * @param cb the callback
 * @param priv a pointer which will be passed to the callback
 * @note iomux_loop() will run the mux (calling iomux_run()) with the
 *       provided timeout. The callback will be called when iomux_run() returns,
 *       just before checking for the leave condition and going ahead calling
 *       iomux_run() again
 */
void iomux_loop_end_cb(iomux_t *iomux, iomux_cb_t cb, void *priv);

/**
 * @brief register the callback which will be called by iomux_loop()
 *        at the end the runcycle if iomux_hangup is set to TRUE
 * @param iomux a valid iomux handler
 * @param cb the callback
 * @param priv a pointer which will be passed to the callback
 */
void iomux_hangup_cb(iomux_t *iomux, iomux_cb_t cb, void *priv);

/**
 * @brief Take over the runloop and handle timeouthandlers while running the mux.
 * @param iomux a valid iomux handler
 */
void iomux_loop(iomux_t *iomux, int timeout);

/**
 * @brief stop a running mux and return control back to the iomux_loop() caller
 * @param iomux a valid iomux handler
 */
void iomux_end_loop(iomux_t *iomux);

/**
 * @brief trigger a runcycle on an iomux
 * @param iomux iomux
 * @param timeout return control to the caller if nothing
 *        happens in the mux within the specified timeout
 */
void iomux_run(iomux_t *iomux, struct timeval *timeout);

/**
 * @brief write to an fd handled by the iomux
 * @param iomux a valid iomux handler
 * @param fd the fd we want to write to
 * @param buf the buffer to write
 * @param len length of the buffer
 * @returns the number of written bytes
 */
int iomux_write(iomux_t *iomux, int fd, const void *buf, int len);

/**
 * @brief close a file handled by the iomux
 * @param iomux a valid iomux handler
 * @param fd the fd to close
 */
void iomux_close(iomux_t *iomux, int fd);

/**
 * @brief relase all resources used by an iomux
 * @param iomux a valid iomux handler
 */
void iomux_destroy(iomux_t *iomux);

/**
 * @brief checks if there is any managed filedescriptor in the iomux instance
 * @param iomux a valid iomux handler
 * @returns TRUE if success; FALSE otherwise
 */
int iomux_isempty(iomux_t *iomux);

#ifdef __cplusplus
}
#endif

#endif
