#include "fuse_ops.hpp"
#include "../in_memory/directory_table.hpp"
#include "../util.hpp"
#include "local_ops.hpp"
#include "remote_ops.hpp"
#include "../rpc/rpc_server.hpp"
#include <cstring>
#include <mutex>
#include <thread>

using namespace std;
#define META_POOL "nmfs.meta"
#define DATA_POOL "nmfs.data"

rados_io *meta_pool;
rados_io *data_pool;

std::unique_ptr<Server> remote_handle;
std::unique_ptr<lease_client> lc;

directory_table *indexing_table;
std::mutex atomic_mutex;

std::map<ino_t, unique_ptr<file_handler>> fh_list;
std::mutex file_handler_mutex;

thread *remote_server_thread;

unsigned int fuse_capable;

void *fuse_ops::init(struct fuse_conn_info *info, struct fuse_config *config) {
	global_logger.log(fuse_op, "Called init()");

	/* argument parsing */
	fuse_context *fuse_ctx = fuse_get_context();
	std::string arg((char *) fuse_ctx->private_data);
	size_t dot_pos = arg.find(',');
	std::string manager_ip = arg.substr(0, dot_pos);
	std::string remote_handle_ip = arg.substr(dot_pos + 1);
	global_logger.log(fuse_op, "manager IP: " + manager_ip + " remote_handle IP: " + remote_handle_ip);

	client *this_client;

	rados_io::conn_info ci = {"client.admin", "ceph", 0};
	meta_pool = new rados_io(ci, META_POOL);
	data_pool = new rados_io(ci, DATA_POOL);

	auto channel = grpc::CreateChannel(manager_ip, grpc::InsecureChannelCredentials());
	lc = std::make_unique<lease_client>(channel, remote_handle_ip);
	this_client = new client(channel);

	global_logger.log(fuse_op, "Client(ID=" + std::to_string(this_client->get_client_id()) + ") is mounted");
	global_logger.log(fuse_op, "Start Inode offset = " + std::to_string(this_client->get_per_client_ino_offset()));

	/* root */
	if (!meta_pool->exist(INODE, "0")) {
		inode i(0, fuse_ctx->gid, S_IFDIR | 0777, true);

		dentry d(0, true);

		i.set_size(DIR_INODE_SIZE);
		i.sync();
		d.sync();
	}

	remote_server_thread = new thread(run_rpc_server, remote_handle_ip);

	indexing_table = new directory_table();

	config->nullpath_ok = 0;
	fuse_capable = info->capable;
	return (void *) this_client;
}

void fuse_ops::destroy(void *private_data) {
	global_logger.log(fuse_op, "Called destroy()");

	delete indexing_table;

	remote_handle->Shutdown();

	fuse_context *fuse_ctx = fuse_get_context();
	client *myself = (client *) (fuse_ctx->private_data);
	delete myself;

	delete meta_pool;
	delete data_pool;
}

int fuse_ops::getattr(const char *path, struct stat *stat, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called getattr()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (std::string(path) == "/" || i->get_loc() == LOCAL) {
			local_getattr(i, stat);
		} else if (i->get_loc() == REMOTE) {
			remote_getattr(std::dynamic_pointer_cast<remote_inode>(i), stat);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return 0;
}

int fuse_ops::access(const char *path, int mask) {
	global_logger.log(fuse_op, "Called access()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (std::string(path) == "/" || i->get_loc() == LOCAL) {
			local_access(i, mask);
		} else if (i->get_loc() == REMOTE) {
			remote_access(std::dynamic_pointer_cast<remote_inode>(i), mask);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		/* from inode constructor */
		return -EACCES;
	}

	return 0;
}

int fuse_ops::symlink(const char *src, const char *dst) {
	global_logger.log(fuse_op, "Called symlink()");
	global_logger.log(fuse_op, "src : " + std::string(src) + " dst : " + std::string(dst));

	int ret = 0;
	try {
		unique_ptr<std::string> dst_parent_name = get_parent_dir_path(dst);
		shared_ptr<inode> dst_parent_i = indexing_table->path_traversal(*dst_parent_name);
		shared_ptr<dentry_table> dst_parent_dentry_table = indexing_table->get_dentry_table(
			dst_parent_i->get_ino());

		if (dst_parent_dentry_table->get_loc() == LOCAL) {
			std::scoped_lock scl{atomic_mutex};
			ret = local_symlink(dst_parent_i, src, dst);
		} else if (dst_parent_dentry_table->get_loc() == REMOTE) {
			std::scoped_lock scl{atomic_mutex};
			shared_ptr<remote_inode> remote_i = std::make_shared<remote_inode>(
				dst_parent_dentry_table->get_leader_ip(),
				dst_parent_dentry_table->get_dir_ino(),
				*dst_parent_name);
			ret = remote_symlink(remote_i, src, dst);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return ret;
}

int fuse_ops::readlink(const char *path, char *buf, size_t size) {
	global_logger.log(fuse_op, "Called readlink()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	int ret = 0;

	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (i->get_loc() == LOCAL) {
			ret = local_readlink(i, buf, size);
		} else if (i->get_loc() == REMOTE) {
			ret = remote_readlink(std::dynamic_pointer_cast<remote_inode>(i), buf, size);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return ret;
}

int fuse_ops::opendir(const char *path, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called opendir()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	int ret = 0;

	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (std::string(path) == "/" || i->get_loc() == LOCAL) {
			ret = local_opendir(i, file_info);
		} else if (i->get_loc() == REMOTE) {
			ret = remote_opendir(std::dynamic_pointer_cast<remote_inode>(i), file_info);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return ret;
}

int fuse_ops::releasedir(const char *path, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called releasedir()");
	int ret = 0;

	if (path != nullptr) {
		global_logger.log(fuse_op, "path : " + std::string(path));
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		ret = local_releasedir(i, file_info);

	} else {
		global_logger.log(fuse_op, "path : nullpath");
		file_handler *fh = reinterpret_cast<file_handler *>(file_info->fh);
		ino_t ino = fh->get_ino();

		ret = local_releasedir(ino, file_info);

	}

	return ret;
}

int fuse_ops::readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset,
		      struct fuse_file_info *file_info, enum fuse_readdir_flags readdir_flags) {
	global_logger.log(fuse_op, "Called readdir()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	shared_ptr<inode> i = indexing_table->path_traversal(path);
	shared_ptr<dentry_table> target_dentry_table = indexing_table->get_dentry_table(i->get_ino());

	if (target_dentry_table->get_loc() == LOCAL) {
		local_readdir(i, buffer, filler);
	} else if (target_dentry_table->get_loc() == REMOTE) {
		shared_ptr<remote_inode> remote_i = std::make_shared<remote_inode>(target_dentry_table->get_leader_ip(),
										   target_dentry_table->get_dir_ino(),
										   *(get_filename_from_path(path)));
		remote_readdir(remote_i, buffer, filler);
	}

	return 0;
}

int fuse_ops::mkdir(const char *path, mode_t mode) {
	global_logger.log(fuse_op, "Called mkdir()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	try {
		std::unique_ptr<std::string> target_name = get_filename_from_path(path);

		shared_ptr<inode> parent_i = indexing_table->path_traversal(*(get_parent_dir_path(path).get()));
		shared_ptr<dentry_table> parent_dentry_table = indexing_table->get_dentry_table(parent_i->get_ino());

		if (parent_dentry_table->get_loc() == LOCAL) {
			std::scoped_lock scl{atomic_mutex};
			ino_t new_dir_ino = local_mkdir(parent_i, *target_name, mode);
			indexing_table->lease_dentry_table(new_dir_ino);
		} else if (parent_dentry_table->get_loc() == REMOTE) {
			std::scoped_lock scl{atomic_mutex};
			shared_ptr<remote_inode> remote_i = std::make_shared<remote_inode>(
				parent_dentry_table->get_leader_ip(),
				parent_dentry_table->get_dir_ino(),
				*target_name);
			ino_t new_dir_ino = remote_mkdir(remote_i, *target_name, mode);
			indexing_table->lease_dentry_table(new_dir_ino);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return 0;
}

int fuse_ops::rmdir(const char *path) {
	global_logger.log(fuse_op, "Called rmdir()");
	global_logger.log(fuse_op, "path : " + std::string(path));
	int ret = 0;

	try {
		std::unique_ptr<std::string> target_name = get_filename_from_path(path);

		shared_ptr<inode> parent_i = indexing_table->path_traversal(*(get_parent_dir_path(path).get()));
		shared_ptr<dentry_table> parent_dentry_table = indexing_table->get_dentry_table(parent_i->get_ino());

		ino_t target_ino = parent_dentry_table->check_child_inode(*target_name);
		shared_ptr<inode> target_i = parent_dentry_table->get_child_inode(*(get_filename_from_path(path).get()),
										  target_ino);
		shared_ptr<dentry_table> target_dentry_table = indexing_table->get_dentry_table(target_i->get_ino());

		if ((parent_dentry_table->get_loc() == LOCAL) && (target_dentry_table->get_loc() == LOCAL)) {
			std::scoped_lock scl{atomic_mutex};
			ret = local_rmdir(parent_i, target_i, *target_name);
		} else if ((parent_dentry_table->get_loc() == LOCAL) && (target_dentry_table->get_loc() == REMOTE)) {
			/* TODO */
		} else if ((parent_dentry_table->get_loc() == REMOTE) && (target_dentry_table->get_loc() == LOCAL)) {
			/* TODO */
		} else if ((parent_dentry_table->get_loc() == REMOTE) && (target_dentry_table->get_loc() == REMOTE)) {
			/* TODO */
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}
	return ret;
}

int fuse_ops::rename(const char *old_path, const char *new_path, unsigned int flags) {
	global_logger.log(fuse_op, "Called rename()");
	global_logger.log(fuse_op, "src : " + std::string(old_path) + " dst : " + std::string(new_path));

	int ret = 0;

	if (std::string(old_path) == std::string(new_path))
		return -EEXIST;

	try {
		unique_ptr<std::string> src_parent_path = get_parent_dir_path(old_path);
		unique_ptr<std::string> dst_parent_path = get_parent_dir_path(new_path);

		if (*src_parent_path == *dst_parent_path) {
			shared_ptr<inode> parent_i = indexing_table->path_traversal(*src_parent_path);
			shared_ptr<dentry_table> parent_dentry_table = indexing_table->get_dentry_table(
				parent_i->get_ino());

			if (parent_dentry_table->get_loc() == LOCAL) {
				std::scoped_lock scl{atomic_mutex};
				ret = local_rename_same_parent(parent_i, old_path, new_path, flags);
			} else if (parent_dentry_table->get_loc() == REMOTE) {
				std::scoped_lock scl{atomic_mutex};
				shared_ptr<remote_inode> remote_i = std::make_shared<remote_inode>(
					parent_dentry_table->get_leader_ip(),
					parent_dentry_table->get_dir_ino(),
					new_path);
				ret = remote_rename_same_parent(remote_i,
								old_path, new_path, flags);
			}


		} else {
			shared_ptr<inode> src_parent_i = indexing_table->path_traversal(*src_parent_path);
			shared_ptr<dentry_table> src_dentry_table = indexing_table->get_dentry_table(
				src_parent_i->get_ino());

			shared_ptr<inode> dst_parent_i = indexing_table->path_traversal(*dst_parent_path);
			shared_ptr<dentry_table> dst_dentry_table = indexing_table->get_dentry_table(
				dst_parent_i->get_ino());

			if ((src_dentry_table->get_loc() == LOCAL) && (dst_dentry_table->get_loc() == LOCAL)) {
				std::scoped_lock scl{atomic_mutex};
				ret = local_rename_not_same_parent(src_parent_i, dst_parent_i, old_path, new_path,
								   flags);
			} else if ((src_dentry_table->get_loc() == LOCAL) && (dst_dentry_table->get_loc() == REMOTE)) {
				/* TODO */
			} else if ((src_dentry_table->get_loc() == REMOTE) && (dst_dentry_table->get_loc() == LOCAL)) {
				/* TODO */
			} else if ((src_dentry_table->get_loc() == REMOTE) && (dst_dentry_table->get_loc() == REMOTE)) {
				/* TODO */
			}

		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return ret;
}

/* file creation flags */

// O_CLOEXEC	- Unimplemented
// O_CREAT	- Handled by the kernel
// O_DIRECTORY	- Handled in this function
// O_EXCL	- Handled by the kernel
// O_NOCTTY	- Handled by the kernel
// O_NOFOLLOW	- Handled in this function
// O_TMPFILE	- Ignored
// O_TRUNC	- Handled in this function

/* file status flags */

// O_APPEND	- Handled in write() (Assume that writeback caching is disabled)
// O_ASYNC	- Ignored
// O_DIRECT	- Ignored
// O_DSYNC	- To be implemented
// O_LARGEFILE	- Ignored
// O_NOATIME	- Ignored
// O_NONBLOCK	- Ignored
// O_PATH	- Unimplemented
// O_SYNC	- To be implemented


int fuse_ops::open(const char *path, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called open()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	/* flags which are unimplemented and to be implemented */

	if (file_info->flags & O_CLOEXEC) throw std::runtime_error("O_CLOEXEC is ON");
	if (file_info->flags & O_PATH) throw std::runtime_error("O_PATH is ON");

	int ret = 0;
	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (i->get_loc() == LOCAL) {
			ret = local_open(i, file_info);
		} else if (i->get_loc() == REMOTE) {
			ret = remote_open(std::dynamic_pointer_cast<remote_inode>(i), file_info);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return ret;
}

int fuse_ops::release(const char *path, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called release()");

	int ret = 0;
	if (path != nullptr) {
		global_logger.log(fuse_op, "path : " + std::string(path));
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		ret = local_release(i, file_info);
	} else {
		global_logger.log(fuse_op, "path : nullpath");
		file_handler *fh = reinterpret_cast<file_handler *>(file_info->fh);
		ino_t ino = fh->get_ino();

		ret = local_release(ino, file_info);
	}
	return ret;
}

int fuse_ops::create(const char *path, mode_t mode, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called create()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	if (S_ISDIR(mode))
		return -EISDIR;

	try {
		std::unique_ptr<std::string> target_name = get_filename_from_path(path);

		shared_ptr<inode> parent_i = indexing_table->path_traversal(*(get_parent_dir_path(path).get()));
		shared_ptr<dentry_table> parent_dentry_table = indexing_table->get_dentry_table(parent_i->get_ino());

		if (parent_dentry_table->get_loc() == LOCAL) {
			std::scoped_lock scl{atomic_mutex};
			local_create(parent_i, *target_name, mode, file_info);
		} else if (parent_dentry_table->get_loc() == REMOTE) {
			std::scoped_lock scl{atomic_mutex};
			shared_ptr<remote_inode> remote_i = std::make_shared<remote_inode>(
				parent_dentry_table->get_leader_ip(),
				parent_dentry_table->get_dir_ino(),
				*target_name);
			remote_create(remote_i, *target_name,
				      mode, file_info);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return 0;
}

int fuse_ops::unlink(const char *path) {
	global_logger.log(fuse_op, "Called unlink()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	try {
		std::unique_ptr<std::string> target_name = get_filename_from_path(path);

		shared_ptr<inode> parent_i = indexing_table->path_traversal(*(get_parent_dir_path(path).get()));
		shared_ptr<dentry_table> parent_dentry_table = indexing_table->get_dentry_table(parent_i->get_ino());

		if (parent_dentry_table->get_loc() == LOCAL) {
			std::scoped_lock scl{atomic_mutex};
			local_unlink(parent_i, *target_name);
		} else if (parent_dentry_table->get_loc() == REMOTE) {
			std::scoped_lock scl{atomic_mutex};
			shared_ptr<remote_inode> remote_i = std::make_shared<remote_inode>(
				parent_dentry_table->get_leader_ip(),
				parent_dentry_table->get_dir_ino(),
				*target_name);
			remote_unlink(remote_i, *target_name);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}
	return 0;
}

int fuse_ops::read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called read()");
	global_logger.log(fuse_op, "path : " + std::string(path) + " size : " + std::to_string(size) + " offset : " +
				   std::to_string(offset));

	size_t read_len = 0;
	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		read_len = local_read(i, buffer, size, offset);
	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	} catch (rados_io::no_such_object &e) {
		return (int) e.num_bytes;
	}

	return (int) read_len;
}

int fuse_ops::write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called write()");
	global_logger.log(fuse_op, "path : " + std::string(path) + " size : " + std::to_string(size) + " offset : " +
				   std::to_string(offset));

	size_t written_len = 0;

	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (i->get_loc() == LOCAL) {
			std::scoped_lock scl{atomic_mutex};
			written_len = local_write(i, buffer, size, offset, file_info->flags);
		} else if (i->get_loc() == REMOTE) {
			std::scoped_lock scl{atomic_mutex};
			written_len = remote_write(std::dynamic_pointer_cast<remote_inode>(i), buffer, size, offset,
						   file_info->flags);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return (int) written_len;
}

int fuse_ops::chmod(const char *path, mode_t mode, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called chmod()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (i->get_loc() == LOCAL) {
			local_chmod(i, mode);
		} else if (i->get_loc() == REMOTE) {
			remote_chmod(std::dynamic_pointer_cast<remote_inode>(i), mode);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return 0;
}

int fuse_ops::chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called chown()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (i->get_loc() == LOCAL) {
			local_chown(i, uid, gid);
		} else if (i->get_loc() == REMOTE) {
			remote_chown(std::dynamic_pointer_cast<remote_inode>(i), uid, gid);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return 0;
}

int fuse_ops::utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *file_info) {
	global_logger.log(fuse_op, "Called utimens()");
	global_logger.log(fuse_op, "path : " + std::string(path));

	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (i->get_loc() == LOCAL) {
			local_utimens(i, tv);
		} else if (i->get_loc() == REMOTE) {
			remote_utimens(std::dynamic_pointer_cast<remote_inode>(i), tv);
		}

	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return 0;
}

int fuse_ops::truncate(const char *path, off_t offset, struct fuse_file_info *fi) {
	global_logger.log(fuse_op, "Called truncate()");
	global_logger.log(fuse_op, "path : " + std::string(path) + " offset : " + std::to_string(offset));

	int ret = 0;
	try {
		shared_ptr<inode> i = indexing_table->path_traversal(path);

		if (i->get_loc() == LOCAL) {
			ret = local_truncate(i, offset);
		} else if (i->get_loc() == REMOTE) {
			ret = remote_truncate(std::dynamic_pointer_cast<remote_inode>(i), offset);
		}
	} catch (inode::no_entry &e) {
		return -ENOENT;
	} catch (inode::permission_denied &e) {
		return -EACCES;
	}

	return ret;
}

fuse_operations fuse_ops::get_fuse_ops(void) {
	fuse_operations fops;
	memset(&fops, 0, sizeof(fuse_operations));

	fops.init = init;
	fops.destroy = destroy;
	fops.getattr = getattr;
	fops.access = access;
	fops.symlink = symlink;
	fops.readlink = readlink;

	fops.opendir = opendir;
	fops.releasedir = releasedir;

	fops.readdir = readdir;
	fops.mkdir = mkdir;
	fops.rmdir = rmdir;
	fops.rename = rename;

	fops.open = open;
	fops.release = release;

	fops.create = create;
	fops.unlink = unlink;

	fops.read = read;
	fops.write = write;

	fops.chmod = chmod;
	fops.chown = chown;
	fops.utimens = utimens;

	fops.truncate = truncate;
	return fops;
}
