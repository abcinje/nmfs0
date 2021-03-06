#ifndef _CHECKPOINT_HPP_
#define _CHECKPOINT_HPP_

#include "mqueue.hpp"
#include "transaction.hpp"

class checkpoint {
private:
	std::shared_ptr<rados_io> meta;
	mqueue<std::shared_ptr<transaction>> *q;

public:
	checkpoint(std::shared_ptr<rados_io> meta_pool, mqueue<std::shared_ptr<transaction>> *queue);
	~checkpoint(void) = default;

	void operator()(void);
};

#endif /* _CHECKPOINT_HPP_ */
