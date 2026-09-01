#include "FileDecrypt.hpp"

#include <fstream>
#include <vector>
#include <algorithm>
#include <memory>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/err.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r {

static const char* const PBKDF2_SALT = "kN2C6si0OK98p7IibGmJ1bQxY9PLWRQi94mPCxqUVgSr5w6YGGXGBaF5bKb";
static const int PBKDF2_KEY_LEN = 32;
static const int PBKDF2_IV_LEN  = 16;
static const int PBKDF2_OUT_LEN = PBKDF2_KEY_LEN + PBKDF2_IV_LEN; // 48

DecryptKeyIV derive_key_iv(const std::string& dev_id, const std::vector<unsigned char>& salt, int iterations)
{
    DecryptKeyIV result;
    result.key.resize(PBKDF2_KEY_LEN);
    result.iv.resize(PBKDF2_IV_LEN);

    std::vector<unsigned char> derived(PBKDF2_OUT_LEN);

    const int ret = PKCS5_PBKDF2_HMAC(
        dev_id.c_str(),
        static_cast<int>(dev_id.size()),
        salt.data(),
        static_cast<int>(salt.size()),
        iterations,
        EVP_sha256(),
        PBKDF2_OUT_LEN,
        derived.data());

    if (ret != 1) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: PKCS5_PBKDF2_HMAC failed";
        result.key.clear();
        result.iv.clear();
        return result;
    }

    std::copy(derived.begin(), derived.begin() + PBKDF2_KEY_LEN, result.key.begin());
    std::copy(derived.begin() + PBKDF2_KEY_LEN, derived.end(), result.iv.begin());

    return result;
}

bool decrypt_file_aes_cbc(const std::string& input_path,
                          const std::string& sn,
                          int iterations,
                          const std::string& output_path)
{
    boost::nowide::ifstream ifs(input_path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: cannot open input file: " << input_path;
        return false;
    }

    const std::streamsize file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    if (file_size < 16) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: file too small for Salted__ header: " << input_path;
        return false;
    }

    std::vector<unsigned char> raw(static_cast<size_t>(file_size));
    if (!ifs.read(reinterpret_cast<char*>(raw.data()), file_size)) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: failed to read input file: " << input_path;
        return false;
    }
    ifs.close();

    // OpenSSL "Salted__" format: [8 "Salted__"] [8 salt] [cipher...]
    static const unsigned char SALTED_MAGIC[8] = { 'S','a','l','t','e','d','_','_' };
    if (std::memcmp(raw.data(), SALTED_MAGIC, 8) != 0) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: missing Salted__ header: " << input_path;
        return false;
    }

    std::vector<unsigned char> salt(raw.begin() + 8, raw.begin() + 16);
    std::vector<unsigned char> cipher_data(raw.begin() + 16, raw.end());

    // password = SN + fixed suffix (PBKDF2 key derivation password, SN is the device serial number)
    static const std::string PASSWORD_SUFFIX = "kN2C6si0OK98p7IibGmJ1bQxY9PLWRQi94mPCxqUVgSr5w6YGGXGBaF5bKb";
    const std::string password = sn + PASSWORD_SUFFIX;
    const DecryptKeyIV dkiv = derive_key_iv(password, salt, iterations);
    if (dkiv.key.empty() || dkiv.iv.empty()) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: key derivation failed";
        return false;
    }

    using EVPCtxGuard = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    EVPCtxGuard ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: EVP_CIPHER_CTX_new failed";
        return false;
    }

    std::vector<unsigned char> plain_data(cipher_data.size() + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;
    int final_len = 0;

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, dkiv.key.data(), dkiv.iv.data()) != 1) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: EVP_DecryptInit_ex failed: "
                                 << ERR_error_string(ERR_get_error(), nullptr);
        return false;
    }

    if (EVP_DecryptUpdate(ctx.get(), plain_data.data(), &out_len,
                          cipher_data.data(), static_cast<int>(cipher_data.size())) != 1) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: EVP_DecryptUpdate failed: "
                                 << ERR_error_string(ERR_get_error(), nullptr);
        return false;
    }

    if (EVP_DecryptFinal_ex(ctx.get(), plain_data.data() + out_len, &final_len) != 1) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: EVP_DecryptFinal_ex failed: "
                                 << ERR_error_string(ERR_get_error(), nullptr);
        return false;
    }

    const int total_len = out_len + final_len;

    boost::nowide::ofstream ofs(output_path, std::ios::binary);
    if (!ofs.is_open()) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: cannot open output file: " << output_path;
        return false;
    }

    ofs.write(reinterpret_cast<const char*>(plain_data.data()), total_len);
    if (ofs.fail()) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: failed to write output file: " << output_path;
        return false;
    }

    return true;
}

} // namespace Slic3r
