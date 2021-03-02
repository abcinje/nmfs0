#ifndef _RADOS_IO_HPP_
#define _RADOS_IO_HPP_

#include <stdexcept>
#include <string>
#include <rados/librados.hpp>

using std::runtime_error;
using std::string;

#define OBJ_SIZE	(4194304)
#define OBJ_BITS	(22)
#define OBJ_MASK	((~0) << OBJ_BITS)

class rados_io {
private:
	librados::Rados cluster;
	librados::IoCtx ioctx;

	size_t read_obj(const string &key, char *value, size_t len, off_t offset);
	size_t write_obj(const string &key, const char *value, size_t len, off_t offset);

public:
	class no_such_object : public runtime_error {
	public:
		no_such_object(const char *msg);
		no_such_object(const string &msg);
		const char *what(void);
	};

	class cannot_acquire_lock : public runtime_error {
	public:
		cannot_acquire_lock(const char *msg);
		cannot_acquire_lock(const string &msg);
		const char *what(void);
	};

	struct conn_info {
		string user;
		string cluster;
		int64_t flags;
	};

	rados_io(const conn_info &ci, string pool);
	~rados_io(void);

	size_t read(const string &key, char *value, size_t len, off_t offset);
	size_t write(const string &key, const char *value, size_t len, off_t offset);
	bool exist(const string &key);
	void remove(const string &key);
};

#endif /* _RADOS_IO_HPP_ */