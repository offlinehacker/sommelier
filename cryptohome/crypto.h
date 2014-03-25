// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Crypto - class for handling the keyset key management functions relating to
// cryptohome.  This includes wrapping/unwrapping the vault keyset (and
// supporting functions) and setting/clearing the user keyring for use with
// ecryptfs.

#ifndef CRYPTOHOME_CRYPTO_H_
#define CRYPTOHOME_CRYPTO_H_

#include <base/basictypes.h>
#include <base/files/file_path.h>
#include <chromeos/secure_blob.h>

#include "tpm.h"
#include "vault_keyset.h"
#include "vault_keyset.pb.h"

namespace cryptohome {

class Crypto {
 public:
  enum CryptoError {
    CE_NONE = 0,
    CE_TPM_FATAL,
    CE_TPM_COMM_ERROR,
    CE_TPM_DEFEND_LOCK,
    CE_TPM_CRYPTO,
    CE_TPM_REBOOT,
    CE_SCRYPT_CRYPTO,
    CE_OTHER_FATAL,
    CE_OTHER_CRYPTO,
    CE_NO_PUBLIC_KEY_HASH,
  };

  // Default constructor
  Crypto(Platform* platform);

  virtual ~Crypto();

  // Initializes Crypto
  bool Init();

  // Decrypts an encrypted vault keyset.  The vault keyset should be the output
  // of EncryptVaultKeyset().
  //
  // Parameters
  //   encrypted_keyset - The blob containing the encrypted keyset
  //   vault_key - The passkey used to decrypt the keyset
  //   crypt_flags (OUT) - Whether the keyset was wrapped by the TPM or scrypt
  //   error (OUT) - The specific error code on failure
  //   vault_keyset (OUT) - The decrypted vault keyset on success
  bool DecryptVaultKeyset(const SerializedVaultKeyset& serialized,
                          const chromeos::SecureBlob& vault_key,
                          unsigned int* crypt_flags, CryptoError* error,
                          VaultKeyset* vault_keyset) const;

  // Encrypts the vault keyset with the given passkey
  //
  // Parameters
  //   vault_keyset - The VaultKeyset to encrypt
  //   vault_key - The passkey used to encrypt the keyset
  //   vault_key_salt - The salt to use for the vault passkey to key conversion
  //                    when encrypting the keyset
  //   encrypted_keyset - On success, the encrypted vault keyset
  bool EncryptVaultKeyset(const VaultKeyset& vault_keyset,
                          const chromeos::SecureBlob& vault_key,
                          const chromeos::SecureBlob& vault_key_salt,
                          SerializedVaultKeyset* serialized) const;

  // Converts the passkey to authorization data for a TPM-backed crypto token.
  //
  // Parameters
  //   passkey - The passkey from which to derive the authorization data.
  //   salt - The salt file used in deriving the authorization data.
  //   auth_data (OUT) - The token authorization data.
  bool PasskeyToTokenAuthData(const chromeos::Blob& passkey,
                              const base::FilePath& salt_file,
                              chromeos::SecureBlob* auth_data) const;

  // Gets an existing salt, or creates one if it doesn't exist
  //
  // Parameters
  //   path - The path to the salt file
  //   length - The length of the new salt if it needs to be created
  //   force - If true, forces creation of a new salt even if the file exists
  //   salt (OUT) - The salt
  bool GetOrCreateSalt(const base::FilePath& path, unsigned int length,
                       bool force, chromeos::SecureBlob* salt) const;

  // Adds the specified key to the ecryptfs keyring so that the cryptohome can
  // be mounted.  Clears the user keyring first.
  //
  // Parameters
  //   vault_keyset - The keyset to add
  //   key_signature (OUT) - The signature of the cryptohome key that should be
  //     used in subsequent calls to mount(2)
  //   filename_key_signature (OUT) - The signature of the cryptohome filename
  //     encryption key that should be used in subsequent calls to mount(2)
  bool AddKeyset(const VaultKeyset& vault_keyset,
                 std::string* key_signature,
                 std::string* filename_key_signature) const;

  // Clears the user's kernel keyring
  void ClearKeyset() const;

  // Converts a null-terminated password to a passkey (ascii-encoded first half
  // of the salted SHA1 hash of the password).
  //
  // Parameters
  //   password - The password to convert
  //   salt - The salt used during hashing
  //   passkey (OUT) - The passkey
  static void PasswordToPasskey(const char* password,
                                const chromeos::Blob& salt,
                                chromeos::SecureBlob* passkey);

  // Ensures that the TPM is connected
  CryptoError EnsureTpm(bool disconnect_first) const;

  // Seals arbitrary-length data to the TPM's PCR0.
  // Parameters
  //   data - Data to encrypt with tpm.
  //   encrypted_data (OUT) - Encrypted data as a string.
  // Returns true if we succeeded in creating the encrypted data blob.
  virtual bool EncryptWithTpm(const chromeos::SecureBlob& data,
                              std::string* encrypted_data) const;

  // Decrypts data previously sealed to the TPM's PCR0.
  // Parameters
  //   encrypted_data - Encrypted data previously sealed with EncryptWithTPM.
  //   data (OUT) - Decrypted data as a blob.
  // Returns true if we succeeded to decrypt the data blob.
  virtual bool DecryptWithTpm(const std::string& encrypted_data,
                              chromeos::SecureBlob* data) const;

  // Note the following 4 methods are only to be used if there is a strong
  // reason to avoid talking to the TPM e.g. needing to flush some encrypted
  // data periodically to disk and you don't want to seal a key each time.
  // Otherwise, a user should use Encrypt/DecryptWithTpm.

  // Creates a randomly generated aes key and seals it to the TPM's PCR0.
  bool CreateSealedKey(chromeos::SecureBlob* aes_key,
                       chromeos::SecureBlob* sealed_key) const;

  // Encrypts the given data using the aes_key. Sealed key is necessary to
  // wrap into the returned data to allow for decryption.
  bool EncryptData(const chromeos::SecureBlob& data,
                   const chromeos::SecureBlob& aes_key,
                   const chromeos::SecureBlob& sealed_key,
                   std::string* encrypted_data) const;

  // Returns the sealed and unsealed aes_key wrapped in the encrypted_data.
  bool UnsealKey(const std::string& encrypted_data,
                 chromeos::SecureBlob* aes_key,
                 chromeos::SecureBlob* sealed_key) const;

  // Decrypts encrypted_data using the aes_key.
  bool DecryptData(const std::string& encrypted_data,
                   const chromeos::SecureBlob& aes_key,
                   chromeos::SecureBlob* data) const;

  // Sets whether or not to use the TPM (must be called before init, depends
  // on the presence of a functioning, initialized TPM).  The TPM is merely used
  // to add a layer of difficulty in a brute-force attack against the user's
  // credentials.
  void set_use_tpm(bool value) {
    use_tpm_ = value;
  }

  // Sets the TPM implementation
  void set_tpm(Tpm* value) {
    tpm_ = value;
  }

  // Gets whether the TPM is set
  bool has_tpm() {
    return (tpm_ != NULL);
  }

  // Gets the TPM implementation
  const Tpm* get_tpm() {
    return tpm_;
  }

  // Checks if the TPM is connected
  bool is_tpm_connected() {
    if (tpm_ == NULL) {
      return false;
    }
    return tpm_->IsConnected();
  }

  // Sets the Platform implementation
  // Does NOT take ownership of the pointer.
  void set_platform(Platform* value) {
    platform_ = value;
  }

  Platform* platform() {
    return platform_;
  }

  static const int64 kSaltMax;

 private:
  // Converts a TPM error to a Crypto error
  CryptoError TpmErrorToCrypto(Tpm::TpmRetryAction retry_action) const;

  // Adds the specified key to the user keyring
  //
  // Parameters
  //   key - The key to add
  //   key_sig - The key's (ascii) signature
  //   salt - The salt
  bool PushVaultKey(const chromeos::SecureBlob& key,
                    const std::string& key_sig,
                    const chromeos::SecureBlob& salt) const;

  bool EncryptTPM(const chromeos::SecureBlob& blob,
                  const chromeos::SecureBlob& key,
                  const chromeos::SecureBlob& salt,
                  SerializedVaultKeyset* serialized) const;

  bool EncryptScrypt(const chromeos::SecureBlob& blob,
                     const chromeos::SecureBlob& key,
                     SerializedVaultKeyset* serialized) const;

  bool DecryptTPM(const SerializedVaultKeyset& serialized,
                  const chromeos::SecureBlob& key,
                  CryptoError* error,
                  VaultKeyset* vault_keyset) const;

  bool DecryptScrypt(const SerializedVaultKeyset& serialized,
                     const chromeos::SecureBlob& key,
                     CryptoError* error,
                     VaultKeyset* keyset) const;

  bool IsTPMPubkeyHash(const std::string& hash, CryptoError* error) const;

  // If set, the TPM will be used during the encryption of the vault keyset
  bool use_tpm_;

  // The TPM implementation
  Tpm* tpm_;

  // Platform abstraction
  Platform* platform_;

  DISALLOW_COPY_AND_ASSIGN(Crypto);
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_CRYPTO_H_
