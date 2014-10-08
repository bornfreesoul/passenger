/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2014 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_APPLICATION_POOL_SESSION_H_
#define _PASSENGER_APPLICATION_POOL_SESSION_H_

#include <sys/types.h>
#include <boost/atomic.hpp>
#include <boost/intrusive_ptr.hpp>
#include <oxt/macros.hpp>
#include <oxt/system_calls.hpp>
#include <oxt/backtrace.hpp>
#include <ApplicationPool2/Common.h>
#include <ApplicationPool2/Socket.h>
#include <FileDescriptor.h>
#include <Utils/ScopeGuard.h>

namespace Passenger {
namespace ApplicationPool2 {

using namespace oxt;


/**
 * Represents a communication session with a process. A communication session
 * within Phusion Passenger is usually a single request + response but the API
 * allows arbitrary I/O. See Process's class overview for normal usage of Session.
 *
 * This class can be used outside the ApplicationPool lock, but is not thread-safe,
 * and so should only be access through 1 thread.
 *
 * You MUST destroy all Session objects before destroying the Pool, because Session
 * objects are stored inside a memory pool inside Pool.
 */
class Session {
public:
	typedef void (*Callback)(Session *session);

private:
	/**
	 * Backpointers to the Pool, Process and Socket that this Session was made
	 * from. `pool` is always non-NULL, but `process` and `socket` are only
	 * non-NULL as long as the Session hasn't been closed. This is because Group
	 * waits until all sessions are closed before destroying a Process.
	 */
	Pool * const pool;
	Process *process;
	Socket *socket;

	Connection connection;
	mutable boost::atomic<int> refcount;
	bool closed;

	void deinitiate(bool success, bool persistent) {
		connection.fail = !success;
		connection.persistent = persistent;
		socket->checkinConnection(connection);
		connection.fd = -1;
	}

	void callOnInitiateFailure() {
		if (OXT_LIKELY(onInitiateFailure != NULL)) {
			onInitiateFailure(this);
		}
	}

	void callOnClose() {
		if (OXT_LIKELY(onClose != NULL)) {
			onClose(this);
		}
		closed = true;
	}

public:
	Callback onInitiateFailure;
	Callback onClose;

	Session(Pool *_pool, Process *_process, Socket *_socket)
		: pool(_pool),
		  process(_process),
		  socket(_socket),
		  refcount(1),
		  closed(false),
		  onInitiateFailure(NULL),
		  onClose(NULL)
		{ }

	~Session() {
		TRACE_POINT();
		// If user doesn't close() explicitly, we penalize performance.
		if (OXT_LIKELY(initiated())) {
			deinitiate(false, false);
		}
		if (OXT_LIKELY(!closed)) {
			callOnClose();
		}
	}

	StaticString getGroupSecret() const;
	pid_t getPid() const;
	StaticString getGupid() const;
	unsigned int getStickySessionId() const;
	Group *getGroup() const;
	void requestOOBW();
	int kill(int signo);
	void destroySelf() const;

	bool isClosed() const {
		return closed;
	}

	Process *getProcess() const {
		assert(!closed);
		return process;
	}

	Socket *getSocket() const {
		assert(!closed);
		return socket;
	}

	StaticString getProtocol() const {
		return getSocket()->protocol;
	}

	void initiate(bool blocking = true) {
		assert(!closed);
		ScopeGuard g(boost::bind(&Session::callOnInitiateFailure, this));
		Connection connection = socket->checkoutConnection();
		connection.fail = true;
		if (connection.blocking && !blocking) {
			FdGuard g2(connection.fd);
			setNonBlocking(connection.fd);
			g2.clear();
			connection.blocking = false;
		}
		g.clear();
		this->connection = connection;
	}

	bool initiated() const {
		return connection.fd != -1;
	}

	OXT_FORCE_INLINE
	int fd() const {
		assert(!closed);
		return connection.fd;
	}

	/**
	 * This Session object becomes fully unsable after closing.
	 */
	void close(bool success, bool persistent = false) {
		if (OXT_LIKELY(initiated())) {
			deinitiate(success, persistent);
		}
		if (OXT_LIKELY(!closed)) {
			callOnClose();
		}
		process = NULL;
		socket  = NULL;
	}

	void ref() const {
		refcount.fetch_add(1, boost::memory_order_relaxed);
	}

	void unref() const {
		if (refcount.fetch_sub(1, boost::memory_order_release) == 1) {
			boost::atomic_thread_fence(boost::memory_order_acquire);
			destroySelf();
		}
	}
};


inline void
intrusive_ptr_add_ref(const Session *session) {
	session->ref();
}

inline void
intrusive_ptr_release(const Session *session) {
	session->unref();
}


} // namespace ApplicationPool2
} // namespace Passenger

#endif /* _PASSENGER_APPLICATION_POOL2_SESSION_H_ */
