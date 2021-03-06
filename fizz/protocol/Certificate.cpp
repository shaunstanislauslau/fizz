/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

#include <fizz/protocol/Certificate.h>

namespace {
int getCurveName(EVP_PKEY* key) {
  auto ecKey = EVP_PKEY_get0_EC_KEY(key);
  if (ecKey) {
    return EC_GROUP_get_curve_name(EC_KEY_get0_group(ecKey));
  }
  return 0;
}
} // namespace

namespace fizz {

Buf CertUtils::prepareSignData(
    CertificateVerifyContext context,
    folly::ByteRange toBeSigned) {
  static constexpr folly::StringPiece kServerLabel =
      "TLS 1.3, server CertificateVerify";
  static constexpr folly::StringPiece kClientLabel =
      "TLS 1.3, client CertificateVerify";
  static constexpr size_t kSigPrefixLen = 64;
  static constexpr uint8_t kSigPrefix = 32;

  folly::StringPiece label;
  if (context == CertificateVerifyContext::Server) {
    label = kServerLabel;
  } else {
    label = kClientLabel;
  }

  size_t sigDataLen = kSigPrefixLen + label.size() + 1 + toBeSigned.size();
  auto buf = folly::IOBuf::create(sigDataLen);
  buf->append(sigDataLen);

  // Place bytes in the right order.
  size_t offset = 0;
  memset(buf->writableData(), kSigPrefix, kSigPrefixLen);
  offset += kSigPrefixLen;
  memcpy(buf->writableData() + offset, label.data(), label.size());
  offset += label.size();
  memset(buf->writableData() + offset, 0, 1);
  offset += 1;
  memcpy(buf->writableData() + offset, toBeSigned.data(), toBeSigned.size());
  return buf;
}

CertificateMsg CertUtils::getCertMessage(
    const std::vector<folly::ssl::X509UniquePtr>& certs,
    Buf certificateRequestContext) {
  // compose the cert entry list
  std::vector<CertificateEntry> entries;
  for (const auto& cert : certs) {
    CertificateEntry entry;
    int len = i2d_X509(cert.get(), nullptr);
    if (len < 0) {
      throw std::runtime_error("Error computing length");
    }
    entry.cert_data = folly::IOBuf::create(len);
    auto dataPtr = entry.cert_data->writableData();
    len = i2d_X509(cert.get(), &dataPtr);
    if (len < 0) {
      throw std::runtime_error("Error converting cert to DER");
    }
    entry.cert_data->append(len);
    // TODO: add any extensions.
    entries.push_back(std::move(entry));
  }

  CertificateMsg msg;
  msg.certificate_request_context = std::move(certificateRequestContext);
  msg.certificate_list = std::move(entries);
  return msg;
}

std::unique_ptr<PeerCert> CertUtils::makePeerCert(Buf certData) {
  if (certData->empty()) {
    throw std::runtime_error("empty peer cert");
  }

  auto range = certData->coalesce();
  const unsigned char* begin = range.data();
  folly::ssl::X509UniquePtr cert(d2i_X509(nullptr, &begin, range.size()));
  if (!cert) {
    throw std::runtime_error("could not read cert");
  }
  if (begin != range.data() + range.size()) {
    VLOG(1) << "Did not read to end of certificate";
  }

  folly::ssl::EvpPkeyUniquePtr pubKey(X509_get_pubkey(cert.get()));
  if (!pubKey) {
    throw std::runtime_error("couldn't get pubkey from peer cert");
  }
  if (EVP_PKEY_id(pubKey.get()) == EVP_PKEY_RSA) {
    return std::make_unique<PeerCertImpl<KeyType::RSA>>(std::move(cert));
  } else if (EVP_PKEY_id(pubKey.get()) == EVP_PKEY_EC) {
    switch (getCurveName(pubKey.get())) {
      case NID_X9_62_prime256v1:
        return std::make_unique<PeerCertImpl<KeyType::P256>>(std::move(cert));
      case NID_secp384r1:
        return std::make_unique<PeerCertImpl<KeyType::P384>>(std::move(cert));
      case NID_secp521r1:
        return std::make_unique<PeerCertImpl<KeyType::P521>>(std::move(cert));
      default:
        break;
    }
  }
  throw std::runtime_error("unknown peer cert type");
}

std::unique_ptr<SelfCert> CertUtils::makeSelfCert(
    std::string certData,
    std::string keyData) {
  auto certs = folly::ssl::OpenSSLCertUtils::readCertsFromBuffer(
      folly::StringPiece(certData));
  if (certs.empty()) {
    throw std::runtime_error("no certificates read");
  }

  folly::ssl::BioUniquePtr b(BIO_new_mem_buf(keyData.data(), keyData.size()));

  if (!b) {
    throw std::runtime_error("failed to create BIO");
  }
  folly::ssl::EvpPkeyUniquePtr key(
      PEM_read_bio_PrivateKey(b.get(), nullptr, nullptr, nullptr));

  if (!key) {
    throw std::runtime_error("Failed to read key");
  }

  return makeSelfCert(std::move(certs), std::move(key));
}

std::unique_ptr<SelfCert> CertUtils::makeSelfCert(
    std::vector<folly::ssl::X509UniquePtr> certs,
    folly::ssl::EvpPkeyUniquePtr key) {
  folly::ssl::EvpPkeyUniquePtr pubKey(X509_get_pubkey(certs.front().get()));
  if (!pubKey) {
    throw std::runtime_error("Failed to read public key");
  }

  if (EVP_PKEY_id(pubKey.get()) == EVP_PKEY_RSA) {
    return std::make_unique<SelfCertImpl<KeyType::RSA>>(
        std::move(key), std::move(certs));
  } else if (EVP_PKEY_id(pubKey.get()) == EVP_PKEY_EC) {
    switch (getCurveName(pubKey.get())) {
      case NID_X9_62_prime256v1:
        return std::make_unique<SelfCertImpl<KeyType::P256>>(
            std::move(key), std::move(certs));
      case NID_secp384r1:
        return std::make_unique<SelfCertImpl<KeyType::P384>>(
            std::move(key), std::move(certs));
      case NID_secp521r1:
        return std::make_unique<SelfCertImpl<KeyType::P521>>(
            std::move(key), std::move(certs));
      default:
        break;
    }
  }
  throw std::runtime_error("unknown self cert type");
}

IdentityCert::IdentityCert(std::string identity) : identity_(identity) {}

std::string IdentityCert::getIdentity() const {
  return identity_;
}

folly::ssl::X509UniquePtr IdentityCert::getX509() const {
  return nullptr;
}
} // namespace fizz
