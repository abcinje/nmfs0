#ifndef _LEASE_TABLE_H_
#define _LEASE_TABLE_H_

#include <chrono>
#include <mutex>
#include <unordered_map>

using namespace std::chrono;

#define LEASE_PERIOD_MS 10000

class lease_table {
private:
	class lease_entry {
	private:
		std::mutex _m;
		system_clock::time_point _due;

	public:
		lease_entry(system_clock::time_point due);
		~lease_entry(void) = default;

		system_clock::time_point get_time(void) const;
		void set_time(system_clock::time_point due);
	};

	std::mutex m;
	std::unordered_map<ino_t, lease_entry *> umap;

public:
	lease_table(void) = default;
	~lease_table(void);

	int acquire(ino_t ino);
};

#endif /* _LEASE_TABLE_H_ */