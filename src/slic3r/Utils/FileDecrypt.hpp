#ifndef __FileDecrypt_hpp__
#define __FileDecrypt_hpp__

#include <string>
#include <vector>

namespace Slic3r {

struct DecryptKeyIV
{
    std::vector<unsigned char> key;
    std::vector<unsigned char> iv;
};

// PBKDF2 iteration count for timelapse decryption. Centralized here so callers
// (DownloadManager) don't hardcode magic numbers at the call site.
static constexpr int PBKDF2_ITERATIONS = 983;

DecryptKeyIV derive_key_iv(const std::string& dev_id, const std::vector<unsigned char>& salt, int iterations);

// Decrypt an OpenSSL "Salted__" file: read salt from the 16-byte header,
// derive key/iv via PBKDF2(password=sn, salt, iterations, SHA256), then
// AES-256-CBC decrypt the payload after the header.
bool decrypt_file_aes_cbc(const std::string& input_path,
                          const std::string& sn,
                          int iterations,
                          const std::string& output_path);

} // namespace Slic3r

#endif // __FileDecrypt_hpp__
