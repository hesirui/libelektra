/**
 * @file
 *
 * @brief filter plugin providing cryptographic operations
 *
 * @copyright BSD License (see LICENSE.md or https://www.libelektra.org)
 *
 */

#ifndef HAVE_KDBCONFIG
#include "kdbconfig.h"
#endif

#include "fcrypt.h"


#include <errno.h>
#include <fcntl.h>
#include <gpg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <kdb.h>
#include <kdberrors.h>
#include <kdbmacros.h>
#include <kdbtypes.h>

enum FcryptGetState
{
	PREGETSTORAGE = 0,
	POSTGETSTORAGE = 1
};

struct _fcryptState
{
	enum FcryptGetState getState;
	struct stat parentStat;
	int parentStatOk;
	int tmpFileFd;
	char * tmpFilePath;
	char * originalFilePath;
};
typedef struct _fcryptState fcryptState;

#define ELEKTRA_FCRYPT_TMP_FILE_SUFFIX "XXXXXX"

/**
 * @brief Allocates a new string holding the name of the temporary file.
 * @param file holds the path to the original file
 * @param fd will hold the file descriptor to the temporary file in case of success
 * @returns an allocated string holding the name of the encrypted file. Must be freed by the caller.
 */
static char * getTemporaryFileName (const char * file, int * fd)
{
	// + 1 to reserve space for the NULL terminator
	const size_t newFileAllocated = strlen (file) + strlen (ELEKTRA_FCRYPT_TMP_FILE_SUFFIX) + 1;
	char * newFile = elektraMalloc (newFileAllocated);
	if (!newFile) return NULL;
	snprintf (newFile, newFileAllocated, "%s" ELEKTRA_FCRYPT_TMP_FILE_SUFFIX, file);
	*fd = mkstemp (newFile);
	return newFile;
}

/**
 * @brief Overwrites the content of the given file with zeroes.
 * @param fd holds the file descriptor to the temporary file to be shredded
 * @param errorKey holds an error description in case of failure
 * @retval 1 on success
 * @retval -1 on failure. In this case errorKey holds an error description.
 */
static int shredTemporaryFile (int fd, Key * errorKey)
{
	kdb_octet_t buffer[512] = { 0 };
	struct stat tmpStat;

	if (fstat (fd, &tmpStat))
	{
		ELEKTRA_SET_ERROR (ELEKTRA_ERROR_FCRYPT_TMP_FILE, errorKey, "Failed to retrieve the file status of the temporary file.");
		return -1;
	}

	if (lseek (fd, 0, SEEK_SET))
	{
		ELEKTRA_SET_ERROR (ELEKTRA_ERROR_FCRYPT_TMP_FILE, errorKey, "Failed to overwrite the temporary file.");
		return -1;
	}

	for (off_t i = 0; i < tmpStat.st_size; i += sizeof (buffer))
	{
		write (fd, buffer, sizeof (buffer));
	}
	return 1;
}

/**
 * @brief lookup if the test mode for unit testing is enabled.
 * @param conf KeySet holding the plugin configuration.
 * @retval 0 test mode is not enabled
 * @retval 1 test mode is enabled
 */
static int inTestMode (KeySet * conf)
{
	Key * k = ksLookupByName (conf, ELEKTRA_CRYPTO_PARAM_GPG_UNIT_TEST, 0);
	if (k && !strcmp (keyString (k), "1"))
	{
		return 1;
	}
	return 0;
}

/**
 * @brief Read number of total GPG recipient keys from the plugin configuration.
 * @param config holds the plugin configuration
 * @param keyName holds the name of the root key to look up
 * @returns the number of GPG recipient keys.
 */
static size_t getRecipientCount (KeySet * config, const char * keyName)
{
	Key * k;
	size_t recipientCount = 0;
	Key * root = ksLookupByName (config, keyName, 0);

	if (!root) return 0;

	// toplevel
	if (strlen (keyString (root)) > 0)
	{
		recipientCount++;
	}

	ksRewind (config);
	while ((k = ksNext (config)) != 0)
	{
		if (keyIsBelow (k, root))
		{
			recipientCount++;
		}
	}
	return recipientCount;
}

/**
 * @brief Determines mtime for the file given by parentKey.
 * @param parentKey holds the path to the file as string value. Is also used for storing warnings.
 * @param fileStat will hold the mtime on success.
 * @retval 1 on success
 * @retval 0 on failure. In this case a warning is appended to parentKey.
 */
static int fcryptSaveMtime (Key * parentKey, struct stat * fileStat)
{
	if (access (keyString (parentKey), F_OK))
	{
		// return failure, so no timestamp is restored later on
		return 0;
	}

	if (stat (keyString (parentKey), fileStat) == -1)
	{
		ELEKTRA_ADD_WARNINGF (29, parentKey, "Failed to read file stats of %s", keyString (parentKey));
		return 0;
	}
	return 1;
}

/**
 * @brief Sets mtime (defined by fileStat) to the file given by parentKey.
 * @param parentKey  holds the path to the file as string value. Is also used for storing warnings.
 * @param fileStat holds the mtime to be set on the file.
 */
static void fcryptRestoreMtime (Key * parentKey, struct stat * fileStat)
{
#if defined(__APPLE__)
	struct timeval times[2];

	// atime - not changing
	times[0].tv_sec = fileStat->st_atime;
	times[0].tv_usec = fileStat->st_atimespec.tv_nsec;

	// mtime
	times[1].tv_sec = ELEKTRA_STAT_SECONDS ((*fileStat));
	times[1].tv_usec = ELEKTRA_STAT_NANO_SECONDS ((*fileStat));

	// restore mtime on parentKeyFd
	if (utimes (keyString (parentKey), times) < 0)
	{
		ELEKTRA_ADD_WARNINGF (ELEKTRA_WARNING_FCRYPT_FUTIMENS, parentKey, "Filename: %s", keyString (parentKey));
	}
#else
	struct timespec times[2];

	// atime - not changing
	times[0].tv_sec = UTIME_OMIT;
	times[0].tv_nsec = UTIME_OMIT;

	// mtime
	times[1].tv_sec = ELEKTRA_STAT_SECONDS ((*fileStat));
	times[1].tv_nsec = ELEKTRA_STAT_NANO_SECONDS ((*fileStat));

	// restore mtime on parentKeyFd
	if (utimensat (AT_FDCWD, keyString (parentKey), times, 0))
	{
		ELEKTRA_ADD_WARNINGF (ELEKTRA_WARNING_FCRYPT_FUTIMENS, parentKey, "Filename: %s", keyString (parentKey));
	}
#endif
}

static int fcryptGpgCallAndCleanup (Key * parentKey, KeySet * pluginConfig, char ** argv, int argc, int tmpFileFd, char * tmpFile)
{
	int parentKeyFd = -1;
	int result = CRYPTO_PLUGIN_FUNCTION (gpgCall) (pluginConfig, parentKey, NULL, argv, argc);

	if (result == 1)
	{
		parentKeyFd = open (keyString (parentKey), O_WRONLY);

		// gpg call returned success, overwrite the original file with the gpg payload data
		if (rename (tmpFile, keyString (parentKey)) != 0)
		{
			ELEKTRA_SET_ERRORF (31, parentKey, "Renaming file %s to %s failed.", tmpFile, keyString (parentKey));
			result = -1;
		}
	}

	if (result == 1)
	{
		if (parentKeyFd >= 0)
		{
			shredTemporaryFile (parentKeyFd, parentKey);
		}
	}
	else
	{
		// if anything went wrong above the temporary file is shredded and removed
		shredTemporaryFile (tmpFileFd, parentKey);
		unlink (tmpFile);
	}

	if (parentKeyFd >= 0)
	{
		close (parentKeyFd);
	}
	close (tmpFileFd);
	elektraFree (tmpFile);
	return result;
}

/**
 * @brief encrypt or sign the file specified at parentKey
 * @param pluginConfig holds the plugin configuration
 * @param parentKey holds the path to the file to be encrypted. Will hold an error description in case of failure.
 * @retval 1 on success
 * @retval -1 on error, errorKey holds an error description
 */
static int fcryptEncrypt (KeySet * pluginConfig, Key * parentKey)
{
	Key * k;
	const size_t recipientCount = getRecipientCount (pluginConfig, ELEKTRA_RECIPIENT_KEY);
	const size_t signatureCount = getRecipientCount (pluginConfig, ELEKTRA_SIGNATURE_KEY);

	if (recipientCount == 0 && signatureCount == 0)
	{
		ELEKTRA_SET_ERRORF (
			ELEKTRA_ERROR_FCRYPT_OPERATION_MODE, parentKey,
			"Missing GPG recipient key (specified as %s) or GPG signature key (specified as %s) in plugin configuration.",
			ELEKTRA_RECIPIENT_KEY, ELEKTRA_SIGNATURE_KEY);
		return -1;
	}

	int tmpFileFd = -1;
	char * tmpFile = getTemporaryFileName (keyString (parentKey), &tmpFileFd);
	if (!tmpFile)
	{
		ELEKTRA_SET_ERROR (87, parentKey, "Memory allocation failed");
		return -1;
	}

	const size_t testMode = inTestMode (pluginConfig);

	// prepare argument vector for gpg call
	// 7 static arguments (magic number below) are:
	//   1. path to the binary
	//   2. --batch
	//   3. -o
	//   4. path to tmp file
	//   5. yes
	//   6. file to be encrypted
	//   7. NULL terminator
	int argc = 7 + (2 * recipientCount) + (2 * signatureCount) + (2 * testMode) + (recipientCount > 0 ? 1 : 0) +
		   (signatureCount > 0 ? 1 : 0);
	kdb_unsigned_short_t i = 0;
	char * argv[argc];
	argv[i++] = NULL;
	argv[i++] = "--batch";
	argv[i++] = "-o";
	argv[i++] = tmpFile;
	argv[i++] = "--yes"; // overwrite files if they exist

	// add recipients
	Key * gpgRecipientRoot = ksLookupByName (pluginConfig, ELEKTRA_RECIPIENT_KEY, 0);

	// append root (gpg/key) as gpg recipient
	if (gpgRecipientRoot && strlen (keyString (gpgRecipientRoot)) > 0)
	{
		argv[i++] = "-r";
		// NOTE argv[] values will not be modified, so const can be discarded safely
		argv[i++] = (char *)keyString (gpgRecipientRoot);
	}

	// append keys beneath root (crypto/key/#_) as gpg recipients
	if (gpgRecipientRoot)
	{
		ksRewind (pluginConfig);
		while ((k = ksNext (pluginConfig)) != 0)
		{
			if (keyIsBelow (k, gpgRecipientRoot))
			{
				argv[i++] = "-r";
				// NOTE argv[] values will not be modified, so const can be discarded safely
				argv[i++] = (char *)keyString (k);
			}
		}
	}


	// add signature keys
	Key * gpgSignatureRoot = ksLookupByName (pluginConfig, ELEKTRA_SIGNATURE_KEY, 0);

	// append root signature key
	if (gpgSignatureRoot && strlen (keyString (gpgSignatureRoot)) > 0)
	{
		argv[i++] = "-u";
		// NOTE argv[] values will not be modified, so const can be discarded safely
		argv[i++] = (char *)keyString (gpgSignatureRoot);
	}

	// append keys beneath root (fcrypt/sign/#_) as gpg signature keys
	if (gpgSignatureRoot)
	{
		ksRewind (pluginConfig);
		while ((k = ksNext (pluginConfig)) != 0)
		{
			if (keyIsBelow (k, gpgSignatureRoot))
			{
				argv[i++] = "-u";
				// NOTE argv[] values will not be modified, so const can be discarded safely
				argv[i++] = (char *)keyString (k);
			}
		}
	}

	// if we are in test mode we add the trust model
	if (testMode > 0)
	{
		argv[i++] = "--trust-model";
		argv[i++] = "always";
	}

	// prepare rest of the argument vector
	if (recipientCount > 0)
	{
		// encrypt the file
		argv[i++] = "-e";
	}
	if (signatureCount > 0)
	{
		// sign the file
		argv[i++] = "-s";
	}
	argv[i++] = (char *)keyString (parentKey);
	argv[i++] = NULL;

	// NOTE the encryption process works like this:
	// gpg2 --batch --yes -o encryptedFile -r keyID -e configFile
	// mv encryptedFile configFile

	return fcryptGpgCallAndCleanup (parentKey, pluginConfig, argv, argc, tmpFileFd, tmpFile);
}

/**
 * @brief decrypt the file specified at parentKey
 * @param pluginConfig holds the plugin configuration
 * @param parentKey holds the path to the file to be encrypted. Will hold an error description in case of failure.
 * @param state holds the plugin state
 * @retval 1 on success
 * @retval -1 on error, errorKey holds an error description
 */
static int fcryptDecrypt (KeySet * pluginConfig, Key * parentKey, fcryptState * state)
{
	int tmpFileFd = -1;
	char * tmpFile = getTemporaryFileName (keyString (parentKey), &tmpFileFd);
	if (!tmpFile)
	{
		ELEKTRA_SET_ERROR (87, parentKey, "Memory allocation failed");
		return -1;
	}

	const size_t testMode = inTestMode (pluginConfig);

	// prepare argument vector for gpg call
	// 8 static arguments (magic number below) are:
	//   1. path to the binary
	//   2. --batch
	//   3. -o
	//   4. path to tmp file
	//   5. yes
	//   6. -d
	//   7. file to be encrypted
	//   8. NULL terminator
	int argc = 8 + (2 * testMode);
	char * argv[argc];
	int i = 0;

	argv[i++] = NULL;
	argv[i++] = "--batch";
	argv[i++] = "--yes";

	// if we are in test mode we add the trust model
	if (testMode)
	{
		argv[i++] = "--trust-model";
		argv[i++] = "always";
	}

	argv[i++] = "-o";
	argv[i++] = tmpFile;
	argv[i++] = "-d";
	// safely discarding const from keyString() return value
	argv[i++] = (char *)keyString (parentKey);
	argv[i++] = NULL;

	// NOTE the decryption process works like this:
	// gpg2 --batch --yes -o tmpfile -d configFile
	int result = CRYPTO_PLUGIN_FUNCTION (gpgCall) (pluginConfig, parentKey, NULL, argv, argc);
	if (result == 1)
	{
		state->originalFilePath = strdup (keyString (parentKey));
		state->tmpFilePath = tmpFile;
		state->tmpFileFd = tmpFileFd;
		keySetString (parentKey, tmpFile);
	}
	else
	{
		// if anything went wrong above the temporary file is shredded and removed
		shredTemporaryFile (tmpFileFd, parentKey);
		unlink (tmpFile);
		close (tmpFileFd);
		elektraFree (tmpFile);
	}
	return result;
}

/**
 * @brief allocates plugin state handle and initializes the plugin state
 * @retval 1 on success
 * @retval -1 on failure
 */
int ELEKTRA_PLUGIN_FUNCTION (ELEKTRA_PLUGIN_NAME, open) (Plugin * handle, KeySet * ks ELEKTRA_UNUSED, Key * parentKey)
{
	fcryptState * s = elektraMalloc (sizeof (fcryptState));
	if (!s)
	{
		ELEKTRA_SET_ERROR (87, parentKey, "Memory allocation failed");
		return -1;
	}

	s->getState = PREGETSTORAGE;
	s->parentStatOk = 0;
	s->tmpFileFd = -1;
	s->tmpFilePath = NULL;
	s->originalFilePath = NULL;

	elektraPluginSetData (handle, s);
	return 1;
}

/**
 * @brief frees the plugin state handle
 * @retval 1 on success
 * @retval -1 on failure
 */
int ELEKTRA_PLUGIN_FUNCTION (ELEKTRA_PLUGIN_NAME, close) (Plugin * handle, KeySet * ks ELEKTRA_UNUSED, Key * parentKey ELEKTRA_UNUSED)
{
	fcryptState * s = (fcryptState *)elektraPluginGetData (handle);
	if (s)
	{
		if (s->tmpFileFd > 0)
		{
			close (s->tmpFileFd);
		}
		if (s->tmpFilePath)
		{
			elektraFree (s->tmpFilePath);
		}
		if (s->originalFilePath)
		{
			elektraFree (s->originalFilePath);
		}
		elektraFree (s);
		elektraPluginSetData (handle, NULL);
	}
	return 1;
}

/**
 * @brief establish the Elektra plugin contract and decrypt the file provided at parentKey using GPG.
 * @retval 1 on success
 * @retval -1 on failure
 */
int ELEKTRA_PLUGIN_FUNCTION (ELEKTRA_PLUGIN_NAME, get) (Plugin * handle, KeySet * ks ELEKTRA_UNUSED, Key * parentKey)
{
	// Publish module configuration to Elektra (establish the contract)
	if (!strcmp (keyName (parentKey), "system/elektra/modules/" ELEKTRA_PLUGIN_NAME))
	{
		KeySet * moduleConfig = ksNew (30,
#include "contract.h"
					       KS_END);
		ksAppend (ks, moduleConfig);
		ksDel (moduleConfig);
		return 1;
	}

	// check plugin state
	KeySet * pluginConfig = elektraPluginGetConfig (handle);
	fcryptState * s = (fcryptState *)elektraPluginGetData (handle);

	if (!s)
	{
		// TODO internal plugin error
		return -1;
	}

	if (s->getState == POSTGETSTORAGE)
	{
		// postgetstorage call will re-direct the parent key to the original encrypted/signed file
		if (s->originalFilePath)
		{
			keySetString (parentKey, s->originalFilePath);
			// TODO check if this is still necessary
			// if(s->parentStatOk)
			//{
			//	fcryptRestoreMtime (parentKey, &(s->parentStat));
			//}
		}
		// TODO else: error if not available

		if (s->tmpFileFd > 0)
		{
			shredTemporaryFile (s->tmpFileFd, parentKey);
			close (s->tmpFileFd);
			s->tmpFileFd = -1;
			unlink (s->tmpFilePath);
			elektraFree (s->tmpFilePath);
			s->tmpFilePath = NULL;
		}
		return 1;
	}

	// now this is a pregetstorage call
	// next time treat the kdb get call as postgetstorage call to trigger encryption after the file has been read
	s->getState = POSTGETSTORAGE;

	// save timestamp of the file
	// TODO verify if this is still required
	s->parentStatOk = fcryptSaveMtime (parentKey, &(s->parentStat));

	return fcryptDecrypt (pluginConfig, parentKey, s);
}

/**
 * @brief Encrypt the file provided at parentKey using GPG.
 * @retval 1 on success
 * @retval -1 on failure
 */
int ELEKTRA_PLUGIN_FUNCTION (ELEKTRA_PLUGIN_NAME, set) (Plugin * handle, KeySet * ks ELEKTRA_UNUSED, Key * parentKey)
{
	KeySet * pluginConfig = elektraPluginGetConfig (handle);
	int encryptionResult = fcryptEncrypt (pluginConfig, parentKey);
	if (encryptionResult != 1) return encryptionResult;

	/* set all keys */
	const char * configFile = keyString (parentKey);
	if (!strcmp (configFile, "")) return 0; // no underlying config file
	int fd = open (configFile, O_RDWR);
	if (fd == -1)
	{
		ELEKTRA_SET_ERRORF (89, parentKey, "Could not open config file %s because %s", configFile, strerror (errno));
		return -1;
	}
	if (fsync (fd) == -1)
	{
		ELEKTRA_SET_ERRORF (89, parentKey, "Could not fsync config file %s because %s", configFile, strerror (errno));
		close (fd);
		return -1;
	}
	close (fd);

	// restore "original" timestamp that has been saved in kdb get / pregetstorage
	// NOTE if we do not restore the timestamp the resolver thinks the file has been changed externally.
	// TODO check if this is still required
	fcryptState * s = (fcryptState *)elektraPluginGetData (handle);
	if (s->parentStatOk)
	{
		fcryptRestoreMtime (parentKey, &(s->parentStat));
	}
	return 1;
}

/**
 * @brief Checks if at least one GPG recipient or at least one GPG signature key hast been provided within the plugin configuration.
 *
 * @retval 0 no changes were made to the configuration
 * @retval 1 the master password has been appended to the configuration
 * @retval -1 an error occurred. Check errorKey
 */
int ELEKTRA_PLUGIN_FUNCTION (ELEKTRA_PLUGIN_NAME, checkconf) (Key * errorKey, KeySet * conf)
{
	const size_t recipientCount = getRecipientCount (conf, ELEKTRA_RECIPIENT_KEY);
	const size_t signatureCount = getRecipientCount (conf, ELEKTRA_SIGNATURE_KEY);

	if (recipientCount == 0 && signatureCount == 0)
	{
		char * errorDescription = CRYPTO_PLUGIN_FUNCTION (getMissingGpgKeyErrorText) (conf);
		ELEKTRA_SET_ERROR (ELEKTRA_ERROR_FCRYPT_OPERATION_MODE, errorKey, errorDescription);
		elektraFree (errorDescription);
		return -1;
	}
	if (CRYPTO_PLUGIN_FUNCTION (gpgVerifyGpgKeysInConfig) (conf, errorKey) != 1)
	{
		// error has been set by CRYPTO_PLUGIN_FUNCTION (gpgVerifyGpgKeysInConfig)
		return -1;
	}
	return 0;
}

Plugin * ELEKTRA_PLUGIN_EXPORT (fcrypt)
{
	// clang-format off
	return elektraPluginExport(ELEKTRA_PLUGIN_NAME,
			ELEKTRA_PLUGIN_OPEN,  &ELEKTRA_PLUGIN_FUNCTION(ELEKTRA_PLUGIN_NAME, open),
			ELEKTRA_PLUGIN_CLOSE, &ELEKTRA_PLUGIN_FUNCTION(ELEKTRA_PLUGIN_NAME, close),
			ELEKTRA_PLUGIN_GET,   &ELEKTRA_PLUGIN_FUNCTION(ELEKTRA_PLUGIN_NAME, get),
			ELEKTRA_PLUGIN_SET,   &ELEKTRA_PLUGIN_FUNCTION(ELEKTRA_PLUGIN_NAME, set),
			ELEKTRA_PLUGIN_END);
}
