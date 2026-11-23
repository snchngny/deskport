#include "identitymanager.h"
#include "utils.h"

#include <QDebug>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/rand.h>

#define SER_UNIQUEID "uniqueid"
#define SER_CERT "certificate"
#define SER_KEY "key"

IdentityManager* IdentityManager::s_Im = nullptr;

IdentityManager*
IdentityManager::get()
{
    // This will always be called first on the main thread,
    // so it's safe to initialize without locks.
    if (s_Im == nullptr) {
        s_Im = new IdentityManager();
    }

    return s_Im;
}

void IdentityManager::createCredentials(QSettings& settings)
{
    X509* cert = X509_new();
    THROW_BAD_ALLOC_IF_NULL(cert);

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    THROW_BAD_ALLOC_IF_NULL(ctx);

    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);

    // pk must be initialized on input
    EVP_PKEY* pk = NULL;
    EVP_PKEY_keygen(ctx, &pk);

    EVP_PKEY_CTX_free(ctx);
    THROW_BAD_ALLOC_IF_NULL(pk);

    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 0);
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60 * 24 * 365 * 20); // 20 yrs
#else
    ASN1_TIME* before = ASN1_STRING_dup(X509_get0_notBefore(cert));
    THROW_BAD_ALLOC_IF_NULL(before);
    ASN1_TIME* after = ASN1_STRING_dup(X509_get0_notAfter(cert));
    THROW_BAD_ALLOC_IF_NULL(after);

    X509_gmtime_adj(before, 0);
    X509_gmtime_adj(after, 60 * 60 * 24 * 365 * 20); // 20 yrs

    X509_set1_notBefore(cert, before);
    X509_set1_notAfter(cert, after);

    ASN1_STRING_free(before);
    ASN1_STRING_free(after);
#endif

    X509_set_pubkey(cert, pk);

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<unsigned char *>(const_cast<char*>("NVIDIA GameStream Client")),
                               -1, -1, 0);
    X509_set_issuer_name(cert, name);

    X509_sign(cert, pk, EVP_sha256());

    BIO* biokey = BIO_new(BIO_s_mem());
    THROW_BAD_ALLOC_IF_NULL(biokey);
    PEM_write_bio_PrivateKey(biokey, pk, NULL, NULL, 0, NULL, NULL);

    BIO* biocert = BIO_new(BIO_s_mem());
    THROW_BAD_ALLOC_IF_NULL(biocert);
    PEM_write_bio_X509(biocert, cert);

    BUF_MEM* mem;
    BIO_get_mem_ptr(biokey, &mem);
    m_CachedPrivateKey = QByteArray(mem->data, (int)mem->length);

    BIO_get_mem_ptr(biocert, &mem);
    m_CachedPemCert = QByteArray(mem->data, (int)mem->length);

    X509_free(cert);
    EVP_PKEY_free(pk);
    BIO_free(biokey);
    BIO_free(biocert);

    // Check that the new keypair is valid before persisting it
    if (getSslCertificate().isNull()) {
        qFatal("Newly generated certificate is unreadable");
    }
    if (getSslKey().isNull()) {
        qFatal("Newly generated private key is unreadable");
    }

    settings.setValue(SER_CERT, m_CachedPemCert);
    settings.setValue(SER_KEY, m_CachedPrivateKey);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        qFatal("Cannot persist the new DeskPort identity. Repair settings access before pairing.");
    }

    qInfo() << "Wrote new identity credentials to settings";
}

IdentityManager::IdentityManager()
{
    QSettings settings;

    m_CachedPemCert = settings.value(SER_CERT).toByteArray();
    m_CachedPrivateKey = settings.value(SER_KEY).toByteArray();

    if (settings.status() != QSettings::NoError) {
        qFatal("Cannot read saved DeskPort identity. Existing credentials were not replaced.");
    }
    const bool existingIdentity = settings.contains(SER_CERT) ||
            settings.contains(SER_KEY) || settings.contains(SER_UNIQUEID);
    if (!existingIdentity) {
        qInfo() << "No existing credentials found";
        createCredentials(settings);
    }
    else if (m_CachedPemCert.isEmpty() || m_CachedPrivateKey.isEmpty()) {
        qFatal("Saved DeskPort identity is incomplete. Existing credentials were not replaced; restore the original settings.");
    }
    else if (getSslCertificate().isNull() || getSslKey().isNull()) {
        qFatal("Saved DeskPort identity cannot be decoded. Existing credentials were not replaced; check the TLS backend or restore the original settings.");
    }

    // We should have valid credentials now. If not, we're screwed
    if (getSslCertificate().isNull()) {
        qFatal("Certificate is unreadable");
    }
    if (getSslKey().isNull()) {
        qFatal("Private key is unreadable");
    }
    BIO* certBio = BIO_new_mem_buf(m_CachedPemCert.constData(), m_CachedPemCert.size());
    THROW_BAD_ALLOC_IF_NULL(certBio);
    X509* certificate = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    BIO_free(certBio);
    BIO* keyBio = BIO_new_mem_buf(m_CachedPrivateKey.constData(), m_CachedPrivateKey.size());
    THROW_BAD_ALLOC_IF_NULL(keyBio);
    EVP_PKEY* key = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
    BIO_free(keyBio);
    const bool matches = certificate && key && X509_check_private_key(certificate, key) == 1;
    X509_free(certificate);
    EVP_PKEY_free(key);
    if (!matches) {
        qFatal("Saved DeskPort certificate and private key do not match. Existing credentials were not replaced.");
    }
}

QSslCertificate
IdentityManager::getSslCertificate()
{
    if (m_CachedSslCert.isNull()) {
        m_CachedSslCert = QSslCertificate(m_CachedPemCert);
    }
    return m_CachedSslCert;
}

QSslKey
IdentityManager::getSslKey()
{
    if (m_CachedSslKey.isNull()) {
        // This seemingly useless const_cast is required for old OpenSSL headers
        // where BIO_new_mem_buf's parameter is not declared const like those on
        // the Steam Link hardware.
        BIO* bio = BIO_new_mem_buf(const_cast<char*>(m_CachedPrivateKey.constData()), -1);
        THROW_BAD_ALLOC_IF_NULL(bio);

        EVP_PKEY* pk = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (pk == nullptr) {
            return QSslKey();
        }

        bio = BIO_new(BIO_s_mem());
        THROW_BAD_ALLOC_IF_NULL(bio);

        // We must write out our PEM in the old PKCS1 format for SecureTransport
        // on macOS/iOS to be able to read it.
#ifdef Q_OS_DARWIN
        PEM_write_bio_PrivateKey_traditional(bio, pk, nullptr, nullptr, 0, nullptr, 0);
#else
        PEM_write_bio_PrivateKey(bio, pk, nullptr, nullptr, 0, nullptr, 0);
#endif

        BUF_MEM* mem;
        BIO_get_mem_ptr(bio, &mem);
        m_CachedSslKey = QSslKey(QByteArray::fromRawData(mem->data, (int)mem->length), QSsl::Rsa);

        BIO_free(bio);
        EVP_PKEY_free(pk);
    }
    return m_CachedSslKey;
}

QSslConfiguration
IdentityManager::getSslConfig()
{
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(getSslCertificate());
    sslConfig.setPrivateKey(getSslKey());
    return sslConfig;
}

QString
IdentityManager::getUniqueId()
{
    if (m_CachedUniqueId.isEmpty()) {
        QSettings settings;

        // Load the unique ID from settings
        m_CachedUniqueId = settings.value(SER_UNIQUEID).toString();
        if (!m_CachedUniqueId.isEmpty()) {
            qInfo() << "Loaded unique ID from settings:" << m_CachedUniqueId;
        }
        else {
            // Generate a new unique ID in base 16
            uint64_t uid;
            RAND_bytes(reinterpret_cast<unsigned char*>(&uid), sizeof(uid));
            m_CachedUniqueId = QString::number(uid, 16);

            qInfo() << "Generated new unique ID:" << m_CachedUniqueId;

            settings.setValue(SER_UNIQUEID, m_CachedUniqueId);
        }
    }
    return m_CachedUniqueId;
}

QByteArray
IdentityManager::getCertificate()
{
    return m_CachedPemCert;
}

QByteArray
IdentityManager::getPrivateKey()
{
    return m_CachedPrivateKey;
}
