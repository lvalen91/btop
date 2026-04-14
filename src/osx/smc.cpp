/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include "smc.hpp"

static constexpr size_t MaxIndexCount = sizeof("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") - 1;
static constexpr const char *KeyIndexes = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static UInt32 _strtoul(char *str, int size, int base) {
	UInt32 total = 0;
	int i;

	for (i = 0; i < size; i++) {
		if (base == 16) {
			total += str[i] << (size - 1 - i) * 8;
		} else {
			total += (unsigned char)(str[i] << (size - 1 - i) * 8);
		}
	}
	return total;
}

static void _ultostr(char *str, UInt32 val) {
	str[0] = '\0';
	snprintf(str, 5, "%c%c%c%c",
			(unsigned int)val >> 24,
			(unsigned int)val >> 16,
			(unsigned int)val >> 8,
			(unsigned int)val);
}

namespace Cpu {

	SMCConnection::SMCConnection() {
		CFMutableDictionaryRef matchingDictionary = IOServiceMatching("AppleSMC");
		result = IOServiceGetMatchingServices(0, matchingDictionary, &iterator);
		if (result != kIOReturnSuccess) {
			throw std::runtime_error("failed to get AppleSMC");
		}

		device = IOIteratorNext(iterator);
		IOObjectRelease(iterator);
		if (device == 0) {
			throw std::runtime_error("failed to get SMC device");
		}

		result = IOServiceOpen(device, mach_task_self(), 0, &conn);
		IOObjectRelease(device);
		if (result != kIOReturnSuccess) {
			throw std::runtime_error("failed to get SMC connection");
		}
	}
	SMCConnection::~SMCConnection() {
		IOServiceClose(conn);
	}

	double SMCConnection::getSMCFloat(const char *key) {
		SMCVal_t val;
		UInt32Char_t k;
		memcpy(k, key, 4);
		k[4] = 0;
		if (SMCReadKey(k, &val) == kIOReturnSuccess and val.dataSize >= 4) {
			float f;
			memcpy(&f, val.bytes, sizeof(f));
			return static_cast<double>(f);
		}
		return -1.0;
	}

	// PC0R: CPU package power in Watts (flt, 4 bytes). Present on Xeon Mac Pro
	// and iMac Pro; also exposed on many consumer Intel Macs.
	double SMCConnection::getCpuPower() {
		return getSMCFloat("PC0R");
	}

	long long SMCConnection::getSMCTemp(char *key) {
		SMCVal_t val;
		kern_return_t result;
		result = SMCReadKey(key, &val);
		if (result == kIOReturnSuccess) {
			if (val.dataSize > 0) {
				if (strcmp(val.dataType, DATATYPE_SP78) == 0) {
					// convert sp78 value to temperature
					int intValue = val.bytes[0] * 256 + (unsigned char)val.bytes[1];
					return static_cast<long long>(intValue / 256.0);
				}
			}
		}
		return -1;
	}

	// core means physical core in SMC, while in core map it's cpu threads :-/ Only an issue on hackintosh?
	// this means we can only get the T per physical core
	// SMC keys are 4 chars, so the index uses a single char: 0-9 then A-Z, matching VirtualSMC's enumeration.
	// This supports up to 36 physical cores (enough for the 28-core Xeon Mac Pro 2019).
	long long SMCConnection::getTemp(int core) {
		char key[] = SMC_KEY_CPU_TEMP;
		if (core < 0) {
			return getSMCTemp(key);
		}
		if ((size_t)core >= MaxIndexCount) {
			return -1;
		}
		snprintf(key, 5, "TC%1cc", KeyIndexes[core]);
		long long result = getSMCTemp(key);
		if (result != -1) return result;
		snprintf(key, 5, "TC%1cC", KeyIndexes[core]);
		result = getSMCTemp(key);
		if (result != -1) return result;
		// Xeon Mac Pro (MacPro7,1) / iMac Pro: no per-physical-core keys exist.
		// Fall back to TCXc, the single "hot core" die sensor these SMCs expose.
		snprintf(key, 5, "TCXc");
		return getSMCTemp(key);
	}

	kern_return_t SMCConnection::SMCReadKey(UInt32Char_t key, SMCVal_t *val) {
		kern_return_t result;
		SMCKeyData_t inputStructure;
		SMCKeyData_t outputStructure;

		memset(&inputStructure, 0, sizeof(SMCKeyData_t));
		memset(&outputStructure, 0, sizeof(SMCKeyData_t));
		memset(val, 0, sizeof(SMCVal_t));

		inputStructure.key = _strtoul(key, 4, 16);
		inputStructure.data8 = SMC_CMD_READ_KEYINFO;

		result = SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure);
		if (result != kIOReturnSuccess)
			return result;

		val->dataSize = outputStructure.keyInfo.dataSize;
		_ultostr(val->dataType, outputStructure.keyInfo.dataType);
		inputStructure.keyInfo.dataSize = val->dataSize;
		inputStructure.data8 = SMC_CMD_READ_BYTES;

		result = SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure);
		if (result != kIOReturnSuccess)
			return result;

		memcpy(val->bytes, outputStructure.bytes, sizeof(outputStructure.bytes));

		return kIOReturnSuccess;
	}

	kern_return_t SMCConnection::SMCCall(int index, SMCKeyData_t *inputStructure, SMCKeyData_t *outputStructure) {
		size_t structureInputSize;
		size_t structureOutputSize;

		structureInputSize = sizeof(SMCKeyData_t);
		structureOutputSize = sizeof(SMCKeyData_t);

		return IOConnectCallStructMethod(conn, index,
										 // inputStructure
										 inputStructure, structureInputSize,
										 // outputStructure
										 outputStructure, &structureOutputSize);
	}

}  // namespace Cpu
