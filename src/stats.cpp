#include "stats.hpp"
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <string.h>


#ifndef PROCDIR
#define PROCDIR "/proc"
#endif

#ifndef PROCSTATFILE
#define PROCSTATFILE PROCDIR "/stat"
#endif

#ifndef PROCMEMINFOFILE
#define PROCMEMINFOFILE PROCDIR "/meminfo"
#endif


static bool starts_with(const std::string& s,  const char *t){
	return s.rfind(t, 0) == 0;
}

CPUStats::CPUStats()
{
	m_inited = Init();
}

bool CPUStats::Init()
{
	std::string line;
	std::ifstream file (PROCSTATFILE);
	bool first = true;
	m_cpuData.clear();

	if (!file.is_open()) {
		std::cerr << "Failed to opening " << PROCSTATFILE << std::endl;
		return false;
	}

	do {
		if (!std::getline(file, line)) {
			std::cerr << "Failed to read all of " << PROCSTATFILE << std::endl;
			file.close();
			return false;
		} else if (starts_with(line, "cpu")) {
			if (first) {
				first =false;
				continue;
			}

			CPUData cpu = {};
			cpu.totalTime = 1;
			cpu.totalPeriod = 1;
			m_cpuData.push_back(cpu);

		} else if (starts_with(line, "btime ")) {

			// C++ way, kind of noisy
			//std::istringstream token( line );
			//std::string s;
			//token >> s;
			//token >> m_boottime;

			// assume that if btime got read, that everything else is OK too
			sscanf(line.c_str(), "btime %lld\n", &m_boottime);
			break;
		}
	} while(true);

	file.close();
	UpdateCPUData();
	return true;
}

//TODO take sampling interval into account?
bool CPUStats::UpdateCPUData()
{
	unsigned long long int usertime, nicetime, systemtime, idletime;
	unsigned long long int ioWait, irq, softIrq, steal, guest, guestnice;
	int cpuid = -1;

	if (!m_inited)
		return false;

	std::string line;
	std::ifstream file (PROCSTATFILE);
	bool ret = false;

	if (!file.is_open()) {
		std::cerr << "Failed to opening " << PROCSTATFILE << std::endl;
		return false;
	}

	do {
		if (!std::getline(file, line)) {
			break;
		} else if (!ret && sscanf(line.c_str(), "cpu  %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu",
			&usertime, &nicetime, &systemtime, &idletime, &ioWait, &irq, &softIrq, &steal, &guest, &guestnice) == 10) {
			ret = true;
		} else if (sscanf(line.c_str(), "cpu%4d %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu",
			&cpuid, &usertime, &nicetime, &systemtime, &idletime, &ioWait, &irq, &softIrq, &steal, &guest, &guestnice) == 11) {

			//std::cerr << "Parsing 'cpu" << cpuid << "' line:" <<  line << std::endl;

			if (!ret) {
				//std::cerr << "Failed to parse 'cpu' line" << std::endl;
				std::cerr << "Failed to parse 'cpu' line:" <<  line << std::endl;
				return false;
			}

			if (cpuid < 0 /* can it? */ || (size_t)cpuid > m_cpuData.size()) {
				std::cerr << "Cpu id '" << cpuid << "' is out of bounds" << std::endl;
				return false;
			}

			CPUData& cpuData = m_cpuData[cpuid];

			// Guest time is already accounted in usertime
			usertime = usertime - guest;
			nicetime = nicetime - guestnice;
			// Fields existing on kernels >= 2.6
			// (and RHEL's patched kernel 2.4...)
			unsigned long long int idlealltime = idletime + ioWait;
			unsigned long long int systemalltime = systemtime + irq + softIrq;
			unsigned long long int virtalltime = guest + guestnice;
			unsigned long long int totaltime = usertime + nicetime + systemalltime + idlealltime + steal + virtalltime;

			// Since we do a subtraction (usertime - guest) and cputime64_to_clock_t()
			// used in /proc/stat rounds down numbers, it can lead to a case where the
			// integer overflow.
			#define WRAP_SUBTRACT(a,b) (a > b) ? a - b : 0
			cpuData.userPeriod = WRAP_SUBTRACT(usertime, cpuData.userTime);
			cpuData.nicePeriod = WRAP_SUBTRACT(nicetime, cpuData.niceTime);
			cpuData.systemPeriod = WRAP_SUBTRACT(systemtime, cpuData.systemTime);
			cpuData.systemAllPeriod = WRAP_SUBTRACT(systemalltime, cpuData.systemAllTime);
			cpuData.idleAllPeriod = WRAP_SUBTRACT(idlealltime, cpuData.idleAllTime);
			cpuData.idlePeriod = WRAP_SUBTRACT(idletime, cpuData.idleTime);
			cpuData.ioWaitPeriod = WRAP_SUBTRACT(ioWait, cpuData.ioWaitTime);
			cpuData.irqPeriod = WRAP_SUBTRACT(irq, cpuData.irqTime);
			cpuData.softIrqPeriod = WRAP_SUBTRACT(softIrq, cpuData.softIrqTime);
			cpuData.stealPeriod = WRAP_SUBTRACT(steal, cpuData.stealTime);
			cpuData.guestPeriod = WRAP_SUBTRACT(virtalltime, cpuData.guestTime);
			cpuData.totalPeriod = WRAP_SUBTRACT(totaltime, cpuData.totalTime);
			#undef WRAP_SUBTRACT
			cpuData.userTime = usertime;
			cpuData.niceTime = nicetime;
			cpuData.systemTime = systemtime;
			cpuData.systemAllTime = systemalltime;
			cpuData.idleAllTime = idlealltime;
			cpuData.idleTime = idletime;
			cpuData.ioWaitTime = ioWait;
			cpuData.irqTime = irq;
			cpuData.softIrqTime = softIrq;
			cpuData.stealTime = steal;
			cpuData.guestTime = virtalltime;
			cpuData.totalTime = totaltime;
			cpuid = -1;
		} else {
			break;
		}
	} while(true);

	file.close();
	m_cpuPeriod = (double)m_cpuData[0].totalPeriod / m_cpuData.size();
	m_updatedCPUs = true;
	return ret;
}

std::string getInputLabel(int index, const char * const sensor, int isensor)
{
	std::ostringstream ss;
	ss << "/sys/class/hwmon/hwmon" << index << "/" << sensor << isensor << "_label";
	return ss.str();
}

std::string getInputPath(int index, const char * const sensor, int isensor)
{
	std::ostringstream ss;
	ss << "/sys/class/hwmon/hwmon" << index << "/" << sensor << isensor << "_input";
	return ss.str();
}

std::string getHwmonPath(int index, const char * const suff)
{
	std::ostringstream ss;
	ss << "/sys/class/hwmon/hwmon" << index << "/" << suff;
	return ss.str();
}

//FIXME un-hard code
AMDgpuStats::AMDgpuStats(int index): m_igpu(index)
{
	Init();
}

bool AMDgpuStats::Init()
{
	int idx = 0;

	std::stringstream str;
	std::string line;
	DIR* dirp;
	struct dirent* dp;

	str << "/sys/class/drm/card" << m_igpu << "/device/hwmon";

	dirp = opendir(str.str().c_str());
	if(dirp == NULL) {
		perror("Error opening drm directory");
		return false;
	}

	m_index = -1;
	bool found_hwmon = false;
	while ((dp = readdir(dirp))) {
		if (starts_with(dp->d_name, "hwmon")) {
			found_hwmon = true;
			break;
		}
	}

	closedir(dirp);

	if (!found_hwmon || sscanf(dp->d_name, "hwmon%d", &m_index) != 1)
		return false;

	std::cerr << "Using hwmon" << m_index << std::endl;

	str.clear(); str.str("");
	str << "/sys/class/hwmon/hwmon" << m_index;
	dirp = opendir(str.str().c_str());
	if(dirp == NULL) {
		perror("Error opening hwmon directory");
		return false;
	}

	while((dp = readdir(dirp)) != NULL)
	{
		static const char * const sensors[] = { "freq", "temp", "fan" };
		for (int i=0; i<3; i++)
		{
			if (starts_with(dp->d_name, sensors[i])) {
				switch(i) {
					case 0:
					case 1:
					{
						char *n = dp->d_name + strlen(dp->d_name) - 5; //label
						if (strcmp(n, "label"))
							continue;

						#ifndef NDEBUG
						std::cout << "hwmon: " << dp->d_name << std::endl;
						#endif

						str.clear(); str.str("");
						str << "/sys/class/hwmon/hwmon" << m_index << "/" << dp->d_name;

						std::ifstream file(str.str());
						if (file.is_open() && std::getline(file, line)
							&& sscanf(dp->d_name + 4, "%d", &idx)) //FIXME
						{
							if (line == "sclk")
								m_isclk = idx;
							else if (line == "mclk")
								m_imclk = idx;
							else if (i == 1 && line == "edge") //TODO polaris only has 'edge'? vega+ adds 'junction'
								m_icore_temp = idx;
							else if (i == 1 && line == "mem")
								m_imem_temp = idx;
						}
					}
					break;
					case 2:
					{
						if (m_ifan < 0 && sscanf(dp->d_name + 3, "%d", &idx) == 1) {
							m_ifan = idx;
						}
					}
					break;
				}
			}
		}
	}

	closedir(dirp);
	return true;
}

int AMDgpuStats::getCoreClock()
{
	std::string line;
	unsigned long long freq = 0;
	std::ifstream file(getInputPath(m_index, "freq", m_isclk));
	if (file.is_open()) {
		std::getline(file, line);
		if (sscanf(line.c_str(), "%llu", &freq) == 1)
			return freq / 1000000;
		return -1;
	}
	return -1;
}

int AMDgpuStats::getMemClock()
{
	std::string line;
	unsigned long long freq = 0;
	std::ifstream file(getInputPath(m_index, "freq", m_imclk));
	if (file.is_open()) {
		std::getline(file, line);
		if (sscanf(line.c_str(), "%llu", &freq) == 1)
			return freq / 1000000;
		return -1;
	}
	return -1;
}

int AMDgpuStats::getCoreTemp()
{
	std::string line;
	unsigned long long value = 0;
	std::ifstream file(getInputPath(m_index, "temp", m_icore_temp));
	if (file.is_open()) {
		std::getline(file, line);
		if (sscanf(line.c_str(), "%llu", &value) == 1)
			return value / 1000;
		return -1;
	}
	return -1;
}

int AMDgpuStats::getMemTemp()
{
	std::string line;
	unsigned long long value = 0;
	std::ifstream file(getInputPath(m_index, "temp", m_imem_temp));
	if (file.is_open()) {
		std::getline(file, line);
		if (sscanf(line.c_str(), "%llu", &value) == 1)
			return value / 1000;
		return -1;
	}
	return -1;
}

int AMDgpuStats::getFanSpeed()
{
	std::string line;
	unsigned long long value = 0;
	std::ifstream file(getInputPath(m_index, "fan", m_ifan));
	if (file.is_open()) {
		std::getline(file, line);
		if (sscanf(line.c_str(), "%llu", &value) == 1)
			return value;
		return -1;
	}
	return -1;
}

int AMDgpuStats::getGPUUsage()
{
	std::string line;
	unsigned long long value = 0;
	std::ifstream file(getHwmonPath(m_index, "device/gpu_busy_percent"));
	if (file.is_open()) {
		std::getline(file, line);
		if (sscanf(line.c_str(), "%llu", &value) == 1)
			return value;
		return -1;
	}
	return -1;
}
