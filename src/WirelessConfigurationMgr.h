
/*
 * WirelessConfigurationMgr.h
 *
 * Manages the storage of Wi-Fi station/AP configuration and credentials.
 */

#ifndef SRC_WIFI_CONFIGURATION_MANAGER_H_
#define SRC_WIFI_CONFIGURATION_MANAGER_H_

#include <cstdint>
#include <cstddef>

#include "include/MessageFormats.h"
#include "esp_partition.h"

class WirelessConfigurationMgr
{
public:
	static constexpr int AP = 0;

	static WirelessConfigurationMgr* GetInstance()
	{
		if (!instance)
		{
			instance = new WirelessConfigurationMgr();
		}
		return instance;
	}

	void Init();
	void Reset(bool format = false);

	int SetSsid(const WirelessConfigurationData& data, bool ap);
	bool EraseSsid(const char *ssid);
	bool GetSsid(int ssid, WirelessConfigurationData& data) const;
	int GetSsid(const char* ssid, WirelessConfigurationData& data) const;

	bool BeginEnterpriseSsid(const WirelessConfigurationData &data);
	bool SetEnterpriseCredential(int cred, const void* buff, size_t size);
	bool EndEnterpriseSsid(bool cancel);
	const uint8_t* GetEnterpriseCredentials(int ssid, const CredentialsInfo& sizes, CredentialsInfo& offsets);

private:
	static WirelessConfigurationMgr* instance;

	static inline constexpr char KVS_PATH[] = "/kvs";
	static inline constexpr char SSIDS_DIR[] = "ssids";

	static inline constexpr char SCRATCH_DIR[] = "scratch";
	static inline constexpr char CREDS_DIR[] = "creds";

	static constexpr int SCRATCH_OFFSET_ID = 0;
	static constexpr int LOADED_SSID_ID = 1;

	static constexpr int MAX_KEY_LEN = 32;

	struct PendingEnterpriseSsid
	{
		int ssid;
		WirelessConfigurationData data;
		CredentialsInfo sizes;
	};

	const esp_partition_t* scratchPartition;
	const uint8_t* scratchBase;

	PendingEnterpriseSsid* pendingSsid;

	bool DeleteKV(const char *key);
	bool SetKV(const char *key, const void *buff, size_t sz, bool append = false);
	bool GetKV(const char *key, void* buff, size_t sz, size_t pos = 0) const;
	size_t GetFree();

	static const char* GetSsidKey(char *buff, int ssid);
	bool SetSsidData(int ssid, const WirelessConfigurationData& data);
	bool EraseSsidData(int ssid);
	bool EraseSsid(int ssid);

	static const char* GetScratchKey(char *buff, int id);
	bool ResetScratch();

	static const char* GetCredentialKey(char* buff, int ssid, int cred);
	bool DeleteCredential(int ssid, int cred);
	bool DeleteCredentials(int ssid);
	bool ResetIfCredentialsLoaded(int ssid);

	int FindEmptySsidEntry() const;
	static bool IsSsidBlank(const WirelessConfigurationData& data);
};

#endif /* SRC_WIFI_CONFIGURATION_MANAGER_H_ */