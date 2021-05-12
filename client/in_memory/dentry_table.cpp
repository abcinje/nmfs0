#include "dentry_table.hpp"

extern std::shared_ptr<inode> root_inode;

dentry_table::dentry_table(ino_t dir_ino, enum meta_location loc) : dir_ino(dir_ino), loc(loc){
	/*
	 * if LOCAL : pull_child_metadata() right after return to caller if dentry object exist
	 * if REMOTE : don't call pull_child_metadata(), just add REMOTE info
	 */
}

dentry_table::~dentry_table() {
	global_logger.log(dentry_table_ops, "Called ~dentry_table(" + std::to_string(this->dir_ino)+")");
	this->child_inodes.clear();
}

int dentry_table::create_child_inode(std::string filename, shared_ptr<inode> inode){
	global_logger.log(dentry_table_ops, "Called create_child_ino(" + filename + ")");
	std::scoped_lock scl{this->dentry_table_mutex};

	auto ret = this->child_inodes.insert(std::make_pair(filename, nullptr));
	if (ret.second) {
		ret.first->second = inode;

		this->dentries->add_new_child(filename, inode->get_ino());
		this->dentries->sync();
	} else {
		global_logger.log(dentry_table_ops, "Already added file is tried to inserted");
		return -1;
	}

	return 0;
}

int dentry_table::add_child_inode(std::string filename, shared_ptr<inode> inode){
	global_logger.log(dentry_table_ops, "Called add_child_ino(" + filename + ")");

	std::scoped_lock scl{this->dentry_table_mutex};
	auto ret = this->child_inodes.insert(std::make_pair(filename, nullptr));
	if(ret.second) {
		ret.first->second = inode;
	} else {
		global_logger.log(dentry_table_ops, "Already added file is tried to inserted");
		return -1;
	}
	return 0;
}

int dentry_table::delete_child_inode(std::string filename) {
	global_logger.log(dentry_table_ops, "Called delete_child_inode(" + filename + ")");
	std::scoped_lock scl{this->dentry_table_mutex};

	std::map<std::string, shared_ptr<inode >>::iterator it;
	it = this->child_inodes.find(filename);

	if (it == this->child_inodes.end()) {
		global_logger.log(dentry_table_ops, "Non-existing file is tried to deleted");
		return -1;
	}

	this->child_inodes.erase(it);

	this->dentries->delete_child(filename);
	this->dentries->sync();

	return 0;
}

shared_ptr<inode> dentry_table::get_child_inode(std::string filename, ino_t for_get_dtable){
	global_logger.log(dentry_table_ops, "Called get_child_inode(" + filename + ", " + std::to_string(for_get_dtable) + ")");
	if(this->get_loc() == LOCAL) {
		std::scoped_lock scl{this->dentry_table_mutex};
		if(filename == "/")
			return root_inode;
		std::map<std::string, shared_ptr<inode>>::iterator it;
		it = this->child_inodes.find(filename);

		if(it == this->child_inodes.end()) {
			throw inode::no_entry("No such file or directory : get_child_inode");
		}
		it->second->set_loc(LOCAL);
		return it->second;
	} else if (this->get_loc() == REMOTE) {
		shared_ptr<remote_inode> remote_i = std::make_shared<remote_inode>(this->leader_ip, this->dir_ino, filename);
		remote_i->inode::set_ino(for_get_dtable);
		remote_i->set_loc(REMOTE);
		return std::dynamic_pointer_cast<inode>(remote_i);
	}

	return nullptr;
}

ino_t dentry_table::check_child_inode(std::string filename){
	global_logger.log(dentry_table_ops, "Called check_child_inode(" + filename + ")");
	if(this->loc == LOCAL) {
		std::scoped_lock scl{this->dentry_table_mutex};
		if(filename == "/")
			return 0;
		std::map<std::string, shared_ptr<inode>>::iterator it;
		it = this->child_inodes.find(filename);

		if(it == this->child_inodes.end()) {
			return -1;
		}

		return it->second->get_ino();
	} else if (this->loc == REMOTE) {
		std::string remote_address(this->leader_ip);
		std::unique_ptr<rpc_client> rc = std::make_unique<rpc_client>(grpc::CreateChannel(remote_address, grpc::InsecureChannelCredentials()));

		ino_t ino = rc->check_child_inode(this->dir_ino, filename);
		return ino;
	}

	return -1;
}

int dentry_table::pull_child_metadata() {
	global_logger.log(dentry_table_ops, "Called pull_child_metadata()");
	std::scoped_lock scl{this->dentry_table_mutex};
	this->dentries = std::make_shared<dentry>(this->dir_ino);

	std::map<std::string, ino_t>::iterator it;
	for(it = this->dentries->child_list.begin(); it != this->dentries->child_list.end(); it++) {
		this->add_child_inode(it->first, std::make_shared<inode>(it->second));
	}

	return 0;
}


enum meta_location dentry_table::get_loc() {
	std::scoped_lock scl{this->dentry_table_mutex};
	return this->loc;
}

void dentry_table::set_leader_id(std::string leader_ip) {
	std::scoped_lock scl{this->dentry_table_mutex};
	this->leader_ip = leader_ip;
}

void dentry_table::set_dentries(shared_ptr<dentry> dentries) {
	std::scoped_lock scl{this->dentry_table_mutex};
	this->dentries = dentries;
}

void dentry_table::fill_filler(void *buffer, fuse_fill_dir_t filler) {
	std::scoped_lock scl{this->dentry_table_mutex};
	this->dentries->fill_filler(buffer, filler);
}

uint64_t dentry_table::get_child_num() {
	std::scoped_lock scl{this->dentry_table_mutex};
	return this->dentries->get_child_num();
}

std::map<std::string, shared_ptr<inode>>::iterator dentry_table::get_child_inode_begin() {
	return this->child_inodes.begin();
}

std::map<std::string, shared_ptr<inode>>::iterator dentry_table::get_child_inode_end() {
	return this->child_inodes.end();
}

ino_t dentry_table::get_dir_ino() const {
	return dir_ino;
}

const string &dentry_table::get_leader_ip() const {
	return leader_ip;
}

std::recursive_mutex &dentry_table::get_dentry_table_mutex() {
	return dentry_table_mutex;
}




