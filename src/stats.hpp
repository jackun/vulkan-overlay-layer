//extern long long btime;
#include <vector>
#include <cstdint>

typedef struct CPUData_ {
	unsigned long long int totalTime;
	unsigned long long int userTime;
	unsigned long long int systemTime;
	unsigned long long int systemAllTime;
	unsigned long long int idleAllTime;
	unsigned long long int idleTime;
	unsigned long long int niceTime;
	unsigned long long int ioWaitTime;
	unsigned long long int irqTime;
	unsigned long long int softIrqTime;
	unsigned long long int stealTime;
	unsigned long long int guestTime;

	unsigned long long int totalPeriod;
	unsigned long long int userPeriod;
	unsigned long long int systemPeriod;
	unsigned long long int systemAllPeriod;
	unsigned long long int idleAllPeriod;
	unsigned long long int idlePeriod;
	unsigned long long int nicePeriod;
	unsigned long long int ioWaitPeriod;
	unsigned long long int irqPeriod;
	unsigned long long int softIrqPeriod;
	unsigned long long int stealPeriod;
	unsigned long long int guestPeriod;
	float percent;
} CPUData;

class IGPUStats
{
	public:
	virtual ~IGPUStats(){}
	virtual int getCoreClock() { return -1; }
	virtual int getMemClock() { return -1; }
	virtual int getShaderClock() { return -1; }
	virtual int getGPUUsage() { return -1; }
	virtual int getMemSize() { return -1; }
	virtual int getMemUsageGlobal() { return -1; }
	// these probably need to return a map with label/value
	virtual int getCoreTemp() { return -1; }
	virtual int getMemTemp() { return -1; }
	virtual int getFanSpeed() { return -1; }
};

class AMDgpuStats: public IGPUStats
{
	public:
	AMDgpuStats(int index);
	~AMDgpuStats(){}
	virtual int getCoreClock();
	virtual int getMemClock();
	virtual int getGPUUsage();
	virtual int getCoreTemp();
	virtual int getMemTemp();
	virtual int getFanSpeed();

	private:
	bool Init();
	int m_index = -1;
	int m_igpu = -1;
	int m_imclk = -1;
	int m_isclk = -1;
	int m_imem_temp = -1;
	int m_icore_temp = -1;
	int m_ifan = -1;
};

class CPUStats
{
public:
	CPUStats();
	bool Init();
	bool Updated()
	{
		return m_updatedCPUs;
	}

	bool UpdateCPUData();
	double GetCPUPeriod() { return m_cpuPeriod; }

	const std::vector<CPUData>& GetCPUData() const {
		return m_cpuData;
	}
private:
	unsigned long long int m_boottime = 0;
	std::vector<CPUData> m_cpuData;
	double m_cpuPeriod = 0;
	bool m_updatedCPUs = false; // TODO use caching or just update?
	bool m_inited = false;
};
