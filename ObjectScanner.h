#define NOMINMAX
#include <Windows.h>
#include <vector>
enum ClassType
{
	Pickup = 0x1c5e258,
	Movable = 0x1c5de18,
	Usable = 0x1c5ec38
};

class ObjectScanner {
private:
	public:
	ObjectScanner();
	~ObjectScanner();
	uintptr_t getCameraTransform();
	std::vector<uintptr_t> scanForType(ClassType typeForScan);

};