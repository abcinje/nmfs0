#ifndef _JOURNAL_TABLE_HPP_
#define _JOURNAL_TABLE_HPP_

#include <memory>
#include <shared_mutex>
#include <tsl/robin_map.h>

#include "transaction.hpp"

class journal_table {
private:
	std::shared_mutex sm;
	tsl::robin_map<ino_t, std::shared_ptr<transaction>> map;

public:
	std::shared_ptr<transaction> get_entry(ino_t ino);
};

#endif /* _JOURNAL_TABLE_HPP_ */