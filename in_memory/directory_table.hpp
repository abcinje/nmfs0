#ifndef NMFS0_DIRECTORY_TABLE_HPP
#define NMFS0_DIRECTORY_TABLE_HPP

#include <map>
#include <utility>
#include <memory>
#include "dentry_table.hpp"
#include "../meta/inode.hpp"
#include "../meta/dentry.hpp"
#include "../logger/logger.hpp"

using std::shared_ptr;

class directory_table {
private:
	std::map<ino_t, shared_ptr<dentry_table>> dentry_tables;

public:
    	int create_table(ino_t ino);
    	int delete_table(ino_t ino);

    	shared_ptr<inode> path_traversal(std::string path);
    	shared_ptr<dentry_table> get_dentry_table(ino_t ino);

};


#endif //NMFS0_DIRECTORY_TABLE_HPP