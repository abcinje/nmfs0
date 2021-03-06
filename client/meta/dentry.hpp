#ifndef NMFS0_DENTRY_HPP
#define NMFS0_DENTRY_HPP

#include <map>
#include <utility>
#include <mutex>

#include <tsl/robin_map.h>

#include "../fs_ops/fuse_ops.hpp"
#include "../../lib/rados_io/rados_io.hpp"
#include "../../lib/logger/logger.hpp"
#include "inode.hpp"

#define MAX_DENTRY_OBJ_SIZE OBJ_SIZE

using std::unique_ptr;
using namespace boost::uuids;

class dentry_table;

/*TODO : when total dentries size exceed single object size */
class dentry {
private:
	uuid this_ino;
	tsl::robin_map<std::string, uuid> child_list;

public:
	explicit dentry(uuid ino, bool mkdir = false);

	void add_child(const std::string &filename, uuid ino);
	void delete_child(const std::string &filename);

	unique_ptr<char[]> serialize();
	void deserialize(char *raw);
	void sync();

	uuid get_child_ino(const std::string& child_name);
	void fill_filler(void *buffer, fuse_fill_dir_t filler);

	uint64_t get_child_num();
	uint64_t get_total_name_length();

	friend class dentry_table;
};


#endif //NMFS0_DENTRY_HPP
