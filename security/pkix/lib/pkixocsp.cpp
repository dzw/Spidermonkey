/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This code is made available to you under your choice of the following sets
 * of licensing terms:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
/* Copyright 2013 Mozilla Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <limits>

#include "pkix/bind.h"
#include "pkix/pkix.h"
#include "pkixcheck.h"
#include "pkixder.h"

namespace mozilla { namespace pkix {

static const PRTime ONE_DAY
  = INT64_C(24) * INT64_C(60) * INT64_C(60) * PR_USEC_PER_SEC;
static const PRTime SLOP = ONE_DAY;

// These values correspond to the tag values in the ASN.1 CertStatus
MOZILLA_PKIX_ENUM_CLASS CertStatus : uint8_t {
  Good = der::CONTEXT_SPECIFIC | 0,
  Revoked = der::CONTEXT_SPECIFIC | der::CONSTRUCTED | 1,
  Unknown = der::CONTEXT_SPECIFIC | 2
};

class Context
{
public:
  Context(TrustDomain& trustDomain, const CertID& certID, PRTime time,
          uint16_t maxLifetimeInDays, /*optional out*/ PRTime* thisUpdate,
          /*optional out*/ PRTime* validThrough)
    : trustDomain(trustDomain)
    , certID(certID)
    , time(time)
    , maxLifetimeInDays(maxLifetimeInDays)
    , certStatus(CertStatus::Unknown)
    , thisUpdate(thisUpdate)
    , validThrough(validThrough)
    , expired(false)
  {
    if (thisUpdate) {
      *thisUpdate = 0;
    }
    if (validThrough) {
      *validThrough = 0;
    }
  }

  TrustDomain& trustDomain;
  const CertID& certID;
  const PRTime time;
  const uint16_t maxLifetimeInDays;
  CertStatus certStatus;
  PRTime* thisUpdate;
  PRTime* validThrough;
  bool expired;

private:
  Context(const Context&); // delete
  void operator=(const Context&); // delete
};

// Verify that potentialSigner is a valid delegated OCSP response signing cert
// according to RFC 6960 section 4.2.2.2.
static Result
CheckOCSPResponseSignerCert(TrustDomain& trustDomain,
                            BackCert& potentialSigner,
                            const SECItem& issuerSubject,
                            const SECItem& issuerSubjectPublicKeyInfo,
                            PRTime time)
{
  Result rv;

  // We don't need to do a complete verification of the signer (i.e. we don't
  // have to call BuildCertChain to verify the entire chain) because we
  // already know that the issuer is valid, since revocation checking is done
  // from the root to the parent after we've built a complete chain that we
  // know is otherwise valid. Rather, we just need to do a one-step validation
  // from potentialSigner to the issuer.
  //
  // It seems reasonable to require the KU_DIGITAL_SIGNATURE key usage on the
  // OCSP responder certificate if the OCSP responder certificate has a
  // key usage extension. However, according to bug 240456, some OCSP responder
  // certificates may have only the nonRepudiation bit set. Also, the OCSP
  // specification (RFC 6960) does not mandate any particular key usage to be
  // asserted for OCSP responde signers. Oddly, the CABForum Baseline
  // Requirements v.1.1.5 do say "If the Root CA Private Key is used for
  // signing OCSP responses, then the digitalSignature bit MUST be set."
  //
  // Note that CheckIssuerIndependentProperties processes
  // SEC_OID_OCSP_RESPONDER in the way that the OCSP specification requires us
  // to--in particular, it doesn't allow SEC_OID_OCSP_RESPONDER to be implied
  // by a missing EKU extension, unlike other EKUs.
  //
  // TODO(bug 926261): If we're validating for a policy then the policy OID we
  // are validating for should be passed to CheckIssuerIndependentProperties.
  rv = CheckIssuerIndependentProperties(trustDomain, potentialSigner, time,
                                        KeyUsage::noParticularKeyUsageRequired,
                                        KeyPurposeId::id_kp_OCSPSigning,
                                        CertPolicyId::anyPolicy, 0);
  if (rv != Success) {
    return rv;
  }

  // It is possible that there exists a certificate with the same key as the
  // issuer but with a different name, so we need to compare names
  // XXX(bug 926270) XXX(bug 1008133) XXX(bug 980163): Improve name
  // comparison.
  // TODO: needs test
  if (!SECITEM_ItemsAreEqual(&potentialSigner.GetIssuer(), &issuerSubject)) {
    return Result::ERROR_OCSP_RESPONDER_CERT_INVALID;
  }

  // TODO(bug 926260): check name constraints
  rv = WrappedVerifySignedData(trustDomain, potentialSigner.GetSignedData(),
                               issuerSubjectPublicKeyInfo);

  // TODO: check for revocation of the OCSP responder certificate unless no-check
  // or the caller forcing no-check. To properly support the no-check policy, we'd
  // need to enforce policy constraints from the issuerChain.

  return rv;
}

MOZILLA_PKIX_ENUM_CLASS ResponderIDType : uint8_t
{
  byName = der::CONTEXT_SPECIFIC | der::CONSTRUCTED | 1,
  byKey = der::CONTEXT_SPECIFIC | der::CONSTRUCTED | 2
};

static inline Result OCSPResponse(Input&, Context&);
static inline Result ResponseBytes(Input&, Context&);
static inline Result BasicResponse(Input&, Context&);
static inline Result ResponseData(
                       Input& tbsResponseData,
                       Context& context,
                       const SignedDataWithSignature& signedResponseData,
                       /*const*/ SECItem* certs, size_t numCerts);
static inline Result SingleResponse(Input& input, Context& context);
static Result ExtensionNotUnderstood(Input& extnID,
                                     const SECItem& extnValue,
                                     bool critical, /*out*/ bool& understood);
static inline Result CertID(Input& input,
                            const Context& context,
                            /*out*/ bool& match);
static Result MatchKeyHash(TrustDomain& trustDomain,
                           const SECItem& issuerKeyHash,
                           const SECItem& issuerSubjectPublicKeyInfo,
                           /*out*/ bool& match);
static Result KeyHash(TrustDomain& trustDomain,
                      const SECItem& subjectPublicKeyInfo,
                      /*out*/ uint8_t* hashBuf, size_t hashBufSize);

static Result
MatchResponderID(TrustDomain& trustDomain,
                 ResponderIDType responderIDType,
                 const SECItem& responderIDItem,
                 const SECItem& potentialSignerSubject,
                 const SECItem& potentialSignerSubjectPublicKeyInfo,
                 /*out*/ bool& match)
{
  match = false;

  switch (responderIDType) {
    case ResponderIDType::byName:
      // XXX(bug 926270) XXX(bug 1008133) XXX(bug 980163): Improve name
      // comparison.
      match = SECITEM_ItemsAreEqual(&responderIDItem, &potentialSignerSubject);
      return Success;

    case ResponderIDType::byKey:
    {
      Input responderID;
      Result rv = responderID.Init(responderIDItem.data, responderIDItem.len);
      if (rv != Success) {
        return rv;
      }
      SECItem keyHash;
      rv = der::ExpectTagAndGetValue(responderID, der::OCTET_STRING, keyHash);
      if (rv != Success) {
        return rv;
      }
      return MatchKeyHash(trustDomain, keyHash,
                          potentialSignerSubjectPublicKeyInfo, match);
    }

    default:
      return Result::ERROR_OCSP_MALFORMED_RESPONSE;
  }
}

static Result
VerifyOCSPSignedData(TrustDomain& trustDomain,
                     const SignedDataWithSignature& signedResponseData,
                     const SECItem& spki)
{
  Result rv = WrappedVerifySignedData(trustDomain, signedResponseData, spki);
  if (rv == Result::ERROR_BAD_SIGNATURE) {
    rv = Result::ERROR_OCSP_BAD_SIGNATURE;
  }
  return rv;
}

// RFC 6960 section 4.2.2.2: The OCSP responder must either be the issuer of
// the cert or it must be a delegated OCSP response signing cert directly
// issued by the issuer. If the OCSP responder is a delegated OCSP response
// signer, then its certificate is (probably) embedded within the OCSP
// response and we'll need to verify that it is a valid certificate that chains
// *directly* to issuerCert.
static Result
VerifySignature(Context& context, ResponderIDType responderIDType,
                const SECItem& responderID, const SECItem* certs,
                size_t numCerts,
                const SignedDataWithSignature& signedResponseData)
{
  bool match;
  Result rv = MatchResponderID(context.trustDomain, responderIDType,
                               responderID, context.certID.issuer,
                               context.certID.issuerSubjectPublicKeyInfo,
                               match);
  if (rv != Success) {
    return rv;
  }
  if (match) {
    return VerifyOCSPSignedData(context.trustDomain, signedResponseData,
                                context.certID.issuerSubjectPublicKeyInfo);
  }

  for (size_t i = 0; i < numCerts; ++i) {
    BackCert cert(certs[i], EndEntityOrCA::MustBeEndEntity, nullptr);
    rv = cert.Init();
    if (rv != Success) {
      return rv;
    }
    rv = MatchResponderID(context.trustDomain, responderIDType, responderID,
                          cert.GetSubject(), cert.GetSubjectPublicKeyInfo(),
                          match);
    if (rv != Success) {
      if (IsFatalError(rv)) {
        return rv;
      }
      continue;
    }

    if (match) {
      rv = CheckOCSPResponseSignerCert(context.trustDomain, cert,
                                       context.certID.issuer,
                                       context.certID.issuerSubjectPublicKeyInfo,
                                       context.time);
      if (rv != Success) {
        if (IsFatalError(rv)) {
          return rv;
        }
        continue;
      }

      return VerifyOCSPSignedData(context.trustDomain, signedResponseData,
                                  cert.GetSubjectPublicKeyInfo());
    }
  }

  return Result::ERROR_OCSP_INVALID_SIGNING_CERT;
}

static inline Result
MapBadDERToMalformedOCSPResponse(Result rv)
{
  if (rv == Result::ERROR_BAD_DER) {
    return Result::ERROR_OCSP_MALFORMED_RESPONSE;
  }
  return rv;
}

Result
VerifyEncodedOCSPResponse(TrustDomain& trustDomain, const struct CertID& certID,
                          PRTime time, uint16_t maxOCSPLifetimeInDays,
                          const SECItem& encodedResponse,
                          /*out*/ bool& expired,
                          /*optional out*/ PRTime* thisUpdate,
                          /*optional out*/ PRTime* validThrough)
{
  // Always initialize this to something reasonable.
  expired = false;

  Input input;
  Result rv = input.Init(encodedResponse.data, encodedResponse.len);
  if (rv != Success) {
    return MapBadDERToMalformedOCSPResponse(rv);
  }

  Context context(trustDomain, certID, time, maxOCSPLifetimeInDays,
                  thisUpdate, validThrough);

  rv = der::Nested(input, der::SEQUENCE, bind(OCSPResponse, _1, ref(context)));
  if (rv != Success) {
    return MapBadDERToMalformedOCSPResponse(rv);
  }

  rv = der::End(input);
  if (rv != Success) {
    return MapBadDERToMalformedOCSPResponse(rv);
  }

  expired = context.expired;

  switch (context.certStatus) {
    case CertStatus::Good:
      if (expired) {
        return Result::ERROR_OCSP_OLD_RESPONSE;
      }
      return Success;
    case CertStatus::Revoked:
      return Result::ERROR_REVOKED_CERTIFICATE;
    case CertStatus::Unknown:
      return Result::ERROR_OCSP_UNKNOWN_CERT;
  }

  PR_NOT_REACHED("unknown CertStatus");
  return Result::ERROR_OCSP_UNKNOWN_CERT;
}

// OCSPResponse ::= SEQUENCE {
//       responseStatus         OCSPResponseStatus,
//       responseBytes          [0] EXPLICIT ResponseBytes OPTIONAL }
//
static inline Result
OCSPResponse(Input& input, Context& context)
{
  // OCSPResponseStatus ::= ENUMERATED {
  //     successful            (0),  -- Response has valid confirmations
  //     malformedRequest      (1),  -- Illegal confirmation request
  //     internalError         (2),  -- Internal error in issuer
  //     tryLater              (3),  -- Try again later
  //                                 -- (4) is not used
  //     sigRequired           (5),  -- Must sign the request
  //     unauthorized          (6)   -- Request unauthorized
  // }
  uint8_t responseStatus;

  Result rv = der::Enumerated(input, responseStatus);
  if (rv != Success) {
    return rv;
  }
  switch (responseStatus) {
    case 0: break; // successful
    case 1: return Result::ERROR_OCSP_MALFORMED_REQUEST;
    case 2: return Result::ERROR_OCSP_SERVER_ERROR;
    case 3: return Result::ERROR_OCSP_TRY_SERVER_LATER;
    case 5: return Result::ERROR_OCSP_REQUEST_NEEDS_SIG;
    case 6: return Result::ERROR_OCSP_UNAUTHORIZED_REQUEST;
    default: return Result::ERROR_OCSP_UNKNOWN_RESPONSE_STATUS;
  }

  return der::Nested(input, der::CONTEXT_SPECIFIC | der::CONSTRUCTED | 0,
                     der::SEQUENCE, bind(ResponseBytes, _1, ref(context)));
}

// ResponseBytes ::=       SEQUENCE {
//     responseType   OBJECT IDENTIFIER,
//     response       OCTET STRING }
static inline Result
ResponseBytes(Input& input, Context& context)
{
  static const uint8_t id_pkix_ocsp_basic[] = {
    0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x30, 0x01, 0x01
  };

  Result rv = der::OID(input, id_pkix_ocsp_basic);
  if (rv != Success) {
    return rv;
  }

  return der::Nested(input, der::OCTET_STRING, der::SEQUENCE,
                     bind(BasicResponse, _1, ref(context)));
}

// BasicOCSPResponse       ::= SEQUENCE {
//    tbsResponseData      ResponseData,
//    signatureAlgorithm   AlgorithmIdentifier,
//    signature            BIT STRING,
//    certs            [0] EXPLICIT SEQUENCE OF Certificate OPTIONAL }
Result
BasicResponse(Input& input, Context& context)
{
  Input tbsResponseData;
  SignedDataWithSignature signedData;
  Result rv = der::SignedData(input, tbsResponseData, signedData);
  if (rv != Success) {
    if (rv == Result::ERROR_BAD_SIGNATURE) {
      return Result::ERROR_OCSP_BAD_SIGNATURE;
    }
    return rv;
  }

  // Parse certificates, if any

  SECItem certs[8];
  size_t numCerts = 0;

  if (!input.AtEnd()) {
    // We ignore the lengths of the wrappers because we'll detect bad lengths
    // during parsing--too short and we'll run out of input for parsing a cert,
    // and too long and we'll have leftover data that won't parse as a cert.

    // [0] wrapper
    rv = der::ExpectTagAndSkipLength(
          input, der::CONTEXT_SPECIFIC | der::CONSTRUCTED | 0);
    if (rv != Success) {
      return rv;
    }

    // SEQUENCE wrapper
    rv = der::ExpectTagAndSkipLength(input, der::SEQUENCE);
    if (rv != Success) {
      return rv;
    }

    // sequence of certificates
    while (!input.AtEnd()) {
      if (numCerts == PR_ARRAY_SIZE(certs)) {
        return Result::ERROR_BAD_DER;
      }

      rv = der::ExpectTagAndGetTLV(input, der::SEQUENCE, certs[numCerts]);
      if (rv != Success) {
        return rv;
      }
      ++numCerts;
    }
  }

  return ResponseData(tbsResponseData, context, signedData, certs, numCerts);
}

// ResponseData ::= SEQUENCE {
//    version             [0] EXPLICIT Version DEFAULT v1,
//    responderID             ResponderID,
//    producedAt              GeneralizedTime,
//    responses               SEQUENCE OF SingleResponse,
//    responseExtensions  [1] EXPLICIT Extensions OPTIONAL }
static inline Result
ResponseData(Input& input, Context& context,
             const SignedDataWithSignature& signedResponseData,
             /*const*/ SECItem* certs, size_t numCerts)
{
  der::Version version;
  Result rv = der::OptionalVersion(input, version);
  if (rv != Success) {
    return rv;
  }
  if (version != der::Version::v1) {
    // TODO: more specific error code for bad version?
    return Result::ERROR_BAD_DER;
  }

  // ResponderID ::= CHOICE {
  //    byName              [1] Name,
  //    byKey               [2] KeyHash }
  SECItem responderID;
  ResponderIDType responderIDType
    = input.Peek(static_cast<uint8_t>(ResponderIDType::byName))
    ? ResponderIDType::byName
    : ResponderIDType::byKey;
  rv = der::ExpectTagAndGetValue(input, static_cast<uint8_t>(responderIDType),
                                 responderID);
  if (rv != Success) {
    return rv;
  }

  // This is the soonest we can verify the signature. We verify the signature
  // right away to follow the principal of minimizing the processing of data
  // before verifying its signature.
  rv = VerifySignature(context, responderIDType, responderID, certs, numCerts,
                       signedResponseData);
  if (rv != Success) {
    return rv;
  }

  // TODO: Do we even need to parse this? Should we just skip it?
  PRTime producedAt;
  rv = der::GeneralizedTime(input, producedAt);
  if (rv != Success) {
    return rv;
  }

  // We don't accept an empty sequence of responses. In practice, a legit OCSP
  // responder will never return an empty response, and handling the case of an
  // empty response makes things unnecessarily complicated.
  rv = der::NestedOf(input, der::SEQUENCE, der::SEQUENCE,
                     der::EmptyAllowed::No,
                     bind(SingleResponse, _1, ref(context)));
  if (rv != Success) {
    return rv;
  }

  return der::OptionalExtensions(input,
                                 der::CONTEXT_SPECIFIC | der::CONSTRUCTED | 1,
                                 ExtensionNotUnderstood);
}

// SingleResponse ::= SEQUENCE {
//    certID                       CertID,
//    certStatus                   CertStatus,
//    thisUpdate                   GeneralizedTime,
//    nextUpdate           [0]     EXPLICIT GeneralizedTime OPTIONAL,
//    singleExtensions     [1]     EXPLICIT Extensions{{re-ocsp-crl |
//                                              re-ocsp-archive-cutoff |
//                                              CrlEntryExtensions, ...}
//                                              } OPTIONAL }
static inline Result
SingleResponse(Input& input, Context& context)
{
  bool match = false;
  Result rv = der::Nested(input, der::SEQUENCE,
                          bind(CertID, _1, cref(context), ref(match)));
  if (rv != Success) {
    return rv;
  }

  if (!match) {
    // This response does not reference the certificate we're interested in.
    // By consuming the rest of our input and returning successfully, we can
    // continue processing and examine another response that might have what
    // we want.
    input.SkipToEnd();
    return Success;
  }

  // CertStatus ::= CHOICE {
  //     good        [0]     IMPLICIT NULL,
  //     revoked     [1]     IMPLICIT RevokedInfo,
  //     unknown     [2]     IMPLICIT UnknownInfo }
  //
  // In the event of multiple SingleResponses for a cert that have conflicting
  // statuses, we use the following precedence rules:
  //
  // * revoked overrides good and unknown
  // * good overrides unknown
  if (input.Peek(static_cast<uint8_t>(CertStatus::Good))) {
    rv = der::ExpectTagAndLength(input, static_cast<uint8_t>(CertStatus::Good),
                                 0);
    if (rv != Success) {
      return rv;
    }
    if (context.certStatus != CertStatus::Revoked) {
      context.certStatus = CertStatus::Good;
    }
  } else if (input.Peek(static_cast<uint8_t>(CertStatus::Revoked))) {
    // We don't need any info from the RevokedInfo structure, so we don't even
    // parse it. TODO: We should mention issues like this in the explanation of
    // why we treat invalid OCSP responses equivalently to revoked for OCSP
    // stapling.
    rv = der::ExpectTagAndSkipValue(input,
                                    static_cast<uint8_t>(CertStatus::Revoked));
    if (rv != Success) {
      return rv;
    }
    context.certStatus = CertStatus::Revoked;
  } else {
    rv = der::ExpectTagAndLength(input,
                                 static_cast<uint8_t>(CertStatus::Unknown), 0);
    if (rv != Success) {
      return rv;
    }
  }

  // http://tools.ietf.org/html/rfc6960#section-3.2
  // 5. The time at which the status being indicated is known to be
  //    correct (thisUpdate) is sufficiently recent;
  // 6. When available, the time at or before which newer information will
  //    be available about the status of the certificate (nextUpdate) is
  //    greater than the current time.

  const PRTime maxLifetime =
    context.maxLifetimeInDays * ONE_DAY;

  PRTime thisUpdate;
  rv = der::GeneralizedTime(input, thisUpdate);
  if (rv != Success) {
    return rv;
  }

  if (thisUpdate > context.time + SLOP) {
    return Result::ERROR_OCSP_FUTURE_RESPONSE;
  }

  PRTime notAfter;
  static const uint8_t NEXT_UPDATE_TAG =
    der::CONTEXT_SPECIFIC | der::CONSTRUCTED | 0;
  if (input.Peek(NEXT_UPDATE_TAG)) {
    PRTime nextUpdate;
    rv = der::Nested(input, NEXT_UPDATE_TAG,
                    bind(der::GeneralizedTime, _1, ref(nextUpdate)));
    if (rv != Success) {
      return rv;
    }

    if (nextUpdate < thisUpdate) {
      return Result::ERROR_OCSP_MALFORMED_RESPONSE;
    }
    if (nextUpdate - thisUpdate <= maxLifetime) {
      notAfter = nextUpdate;
    } else {
      notAfter = thisUpdate + maxLifetime;
    }
  } else {
    // NSS requires all OCSP responses without a nextUpdate to be recent.
    // Match that stricter behavior.
    notAfter = thisUpdate + ONE_DAY;
  }

  if (context.time < SLOP) { // prevent underflow
    return Result::FATAL_ERROR_INVALID_ARGS;
  }

  if (context.time - SLOP > notAfter) {
    context.expired = true;
  }

  rv = der::OptionalExtensions(input,
                               der::CONTEXT_SPECIFIC | der::CONSTRUCTED | 1,
                               ExtensionNotUnderstood);
  if (rv != Success) {
    return rv;
  }

  if (context.thisUpdate) {
    *context.thisUpdate = thisUpdate;
  }
  if (context.validThrough) {
    *context.validThrough = notAfter;
  }

  return Success;
}

// CertID          ::=     SEQUENCE {
//        hashAlgorithm       AlgorithmIdentifier,
//        issuerNameHash      OCTET STRING, -- Hash of issuer's DN
//        issuerKeyHash       OCTET STRING, -- Hash of issuer's public key
//        serialNumber        CertificateSerialNumber }
static inline Result
CertID(Input& input, const Context& context, /*out*/ bool& match)
{
  match = false;

  DigestAlgorithm hashAlgorithm;
  Result rv = der::DigestAlgorithmIdentifier(input, hashAlgorithm);
  if (rv != Success) {
    if (rv == Result::ERROR_INVALID_ALGORITHM) {
      // Skip entries that are hashed with algorithms we don't support.
      input.SkipToEnd();
      return Success;
    }
    return rv;
  }

  SECItem issuerNameHash;
  rv = der::ExpectTagAndGetValue(input, der::OCTET_STRING, issuerNameHash);
  if (rv != Success) {
    return rv;
  }

  SECItem issuerKeyHash;
  rv = der::ExpectTagAndGetValue(input, der::OCTET_STRING, issuerKeyHash);
  if (rv != Success) {
    return rv;
  }

  SECItem serialNumber;
  rv = der::CertificateSerialNumber(input, serialNumber);
  if (rv != Success) {
    return rv;
  }

  if (!SECITEM_ItemsAreEqual(&serialNumber, &context.certID.serialNumber)) {
    // This does not reference the certificate we're interested in.
    // Consume the rest of the input and return successfully to
    // potentially continue processing other responses.
    input.SkipToEnd();
    return Success;
  }

  // TODO: support SHA-2 hashes.

  if (hashAlgorithm != DigestAlgorithm::sha1) {
    // Again, not interested in this response. Consume input, return success.
    input.SkipToEnd();
    return Success;
  }

  if (issuerNameHash.len != TrustDomain::DIGEST_LENGTH) {
    return Result::ERROR_OCSP_MALFORMED_RESPONSE;
  }

  // From http://tools.ietf.org/html/rfc6960#section-4.1.1:
  // "The hash shall be calculated over the DER encoding of the
  // issuer's name field in the certificate being checked."
  uint8_t hashBuf[TrustDomain::DIGEST_LENGTH];
  rv = context.trustDomain.DigestBuf(context.certID.issuer, hashBuf,
                                     sizeof(hashBuf));
  if (rv != Success) {
    return rv;
  }
  if (memcmp(hashBuf, issuerNameHash.data, issuerNameHash.len)) {
    // Again, not interested in this response. Consume input, return success.
    input.SkipToEnd();
    return Success;
  }

  return MatchKeyHash(context.trustDomain, issuerKeyHash,
                      context.certID.issuerSubjectPublicKeyInfo, match);
}

// From http://tools.ietf.org/html/rfc6960#section-4.1.1:
// "The hash shall be calculated over the value (excluding tag and length) of
// the subject public key field in the issuer's certificate."
//
// From http://tools.ietf.org/html/rfc6960#appendix-B.1:
// KeyHash ::= OCTET STRING -- SHA-1 hash of responder's public key
//                          -- (i.e., the SHA-1 hash of the value of the
//                          -- BIT STRING subjectPublicKey [excluding
//                          -- the tag, length, and number of unused
//                          -- bits] in the responder's certificate)
static Result
MatchKeyHash(TrustDomain& trustDomain, const SECItem& keyHash,
             const SECItem& subjectPublicKeyInfo, /*out*/ bool& match)
{
  if (keyHash.len != TrustDomain::DIGEST_LENGTH)  {
    return Result::ERROR_OCSP_MALFORMED_RESPONSE;
  }
  static uint8_t hashBuf[TrustDomain::DIGEST_LENGTH];
  Result rv = KeyHash(trustDomain, subjectPublicKeyInfo, hashBuf,
                      sizeof hashBuf);
  if (rv != Success) {
    return rv;
  }
  match = !memcmp(hashBuf, keyHash.data, keyHash.len);
  return Success;
}

// TODO(bug 966856): support SHA-2 hashes
Result
KeyHash(TrustDomain& trustDomain, const SECItem& subjectPublicKeyInfo,
        /*out*/ uint8_t* hashBuf, size_t hashBufSize)
{
  if (!hashBuf || hashBufSize != TrustDomain::DIGEST_LENGTH) {
    return Result::FATAL_ERROR_LIBRARY_FAILURE;
  }

  // RFC 5280 Section 4.1
  //
  // SubjectPublicKeyInfo  ::=  SEQUENCE  {
  //    algorithm            AlgorithmIdentifier,
  //    subjectPublicKey     BIT STRING  }

  Input spki;
  Result rv;

  {
    // The scope of input is limited to reduce the possibility of confusing it
    // with spki in places we need to be using spki below.
    Input input;
    rv = input.Init(subjectPublicKeyInfo.data, subjectPublicKeyInfo.len);
    if (rv != Success) {
      return rv;
    }

    rv = der::ExpectTagAndGetValue(input, der::SEQUENCE, spki);
    if (rv != Success) {
      return rv;
    }
    rv = der::End(input);
    if (rv != Success) {
      return rv;
    }
  }

  // Skip AlgorithmIdentifier
  rv = der::ExpectTagAndSkipValue(spki, der::SEQUENCE);
  if (rv != Success) {
    return rv;
  }

  SECItem subjectPublicKey;
  rv = der::ExpectTagAndGetValue(spki, der::BIT_STRING, subjectPublicKey);
  if (rv != Success) {
    return rv;
  }

  rv = der::End(spki);
  if (rv != Success) {
    return rv;
  }

  // Assume/require that the number of unused bits in the public key is zero.
  if (subjectPublicKey.len == 0 || subjectPublicKey.data[0] != 0) {
    return Result::ERROR_BAD_DER;
  }
  ++subjectPublicKey.data;
  --subjectPublicKey.len;

  return trustDomain.DigestBuf(subjectPublicKey, hashBuf, hashBufSize);
}

Result
ExtensionNotUnderstood(Input& /*extnID*/, const SECItem& /*extnValue*/,
                       bool /*critical*/, /*out*/ bool& understood)
{
  understood = false;
  return Success;
}

//   1. The certificate identified in a received response corresponds to
//      the certificate that was identified in the corresponding request;
//   2. The signature on the response is valid;
//   3. The identity of the signer matches the intended recipient of the
//      request;
//   4. The signer is currently authorized to provide a response for the
//      certificate in question;
//   5. The time at which the status being indicated is known to be
//      correct (thisUpdate) is sufficiently recent;
//   6. When available, the time at or before which newer information will
//      be available about the status of the certificate (nextUpdate) is
//      greater than the current time.
//
//   Responses whose nextUpdate value is earlier than
//   the local system time value SHOULD be considered unreliable.
//   Responses whose thisUpdate time is later than the local system time
//   SHOULD be considered unreliable.
//
//   If nextUpdate is not set, the responder is indicating that newer
//   revocation information is available all the time.
//
// http://tools.ietf.org/html/rfc5019#section-4

Result
CreateEncodedOCSPRequest(TrustDomain& trustDomain, const struct CertID& certID,
                         /*out*/ uint8_t (&out)[OCSP_REQUEST_MAX_LENGTH],
                         /*out*/ size_t& outLen)
{
  // We do not add any extensions to the request.

  // RFC 6960 says "An OCSP client MAY wish to specify the kinds of response
  // types it understands. To do so, it SHOULD use an extension with the OID
  // id-pkix-ocsp-response." This use of MAY and SHOULD is unclear. MSIE11
  // on Windows 8.1 does not include any extensions, whereas NSS has always
  // included the id-pkix-ocsp-response extension. Avoiding the sending the
  // extension is better for OCSP GET because it makes the request smaller,
  // and thus more likely to fit within the 255 byte limit for OCSP GET that
  // is specified in RFC 5019 Section 5.

  // Bug 966856: Add the id-pkix-ocsp-pref-sig-algs extension.

  // Since we don't know whether the OCSP responder supports anything other
  // than SHA-1, we have no choice but to use SHA-1 for issuerNameHash and
  // issuerKeyHash.
  static const uint8_t hashAlgorithm[11] = {
    0x30, 0x09,                               // SEQUENCE
    0x06, 0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A, //   OBJECT IDENTIFIER id-sha1
    0x05, 0x00,                               //   NULL
  };
  static const uint8_t hashLen = TrustDomain::DIGEST_LENGTH;

  static const unsigned int totalLenWithoutSerialNumberData
    = 2                             // OCSPRequest
    + 2                             //   tbsRequest
    + 2                             //     requestList
    + 2                             //       Request
    + 2                             //         reqCert (CertID)
    + PR_ARRAY_SIZE(hashAlgorithm)  //           hashAlgorithm
    + 2 + hashLen                   //           issuerNameHash
    + 2 + hashLen                   //           issuerKeyHash
    + 2;                            //           serialNumber (header)

  // The only way we could have a request this large is if the serialNumber was
  // ridiculously and unreasonably large. RFC 5280 says "Conforming CAs MUST
  // NOT use serialNumber values longer than 20 octets." With this restriction,
  // we allow for some amount of non-conformance with that requirement while
  // still ensuring we can encode the length values in the ASN.1 TLV structures
  // in a single byte.
  static_assert(totalLenWithoutSerialNumberData < OCSP_REQUEST_MAX_LENGTH,
                "totalLenWithoutSerialNumberData too big");
  if (certID.serialNumber.len >
        OCSP_REQUEST_MAX_LENGTH - totalLenWithoutSerialNumberData) {
    return Result::ERROR_BAD_DER;
  }

  outLen = totalLenWithoutSerialNumberData + certID.serialNumber.len;

  uint8_t totalLen = static_cast<uint8_t>(outLen);

  uint8_t* d = out;
  *d++ = 0x30; *d++ = totalLen - 2u;  // OCSPRequest (SEQUENCE)
  *d++ = 0x30; *d++ = totalLen - 4u;  //   tbsRequest (SEQUENCE)
  *d++ = 0x30; *d++ = totalLen - 6u;  //     requestList (SEQUENCE OF)
  *d++ = 0x30; *d++ = totalLen - 8u;  //       Request (SEQUENCE)
  *d++ = 0x30; *d++ = totalLen - 10u; //         reqCert (CertID SEQUENCE)

  // reqCert.hashAlgorithm
  for (size_t i = 0; i < PR_ARRAY_SIZE(hashAlgorithm); ++i) {
    *d++ = hashAlgorithm[i];
  }

  // reqCert.issuerNameHash (OCTET STRING)
  *d++ = 0x04;
  *d++ = hashLen;
  Result rv = trustDomain.DigestBuf(certID.issuer, d, hashLen);
  if (rv != Success) {
    return rv;
  }
  d += hashLen;

  // reqCert.issuerKeyHash (OCTET STRING)
  *d++ = 0x04;
  *d++ = hashLen;
  rv = KeyHash(trustDomain, certID.issuerSubjectPublicKeyInfo, d, hashLen);
  if (rv != Success) {
    return rv;
  }
  d += hashLen;

  // reqCert.serialNumber (INTEGER)
  *d++ = 0x02; // INTEGER
  *d++ = static_cast<uint8_t>(certID.serialNumber.len);
  for (size_t i = 0; i < certID.serialNumber.len; ++i) {
    *d++ = certID.serialNumber.data[i];
  }

  PR_ASSERT(d == out + totalLen);

  return Success;
}

} } // namespace mozilla::pkix
