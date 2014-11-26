/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>

#include <keymaster/google_keymaster_utils.h>
#include <keymaster/key_blob.h>

namespace keymaster {

const size_t KeyBlob::NONCE_LENGTH;
const size_t KeyBlob::TAG_LENGTH;

KeyBlob::KeyBlob(const uint8_t* key_blob, size_t key_blob_length) : error_(KM_ERROR_OK) {
    Deserialize(&key_blob, key_blob + key_blob_length);
}

KeyBlob::KeyBlob(const keymaster_key_blob_t& key_blob) : error_(KM_ERROR_OK) {
    const uint8_t* key_material = key_blob.key_material;
    Deserialize(&key_material, key_blob.key_material + key_blob.key_material_size);
}

size_t KeyBlob::SerializedSize() const {
    return 1 /* version byte */ + sizeof(uint32_t) /* nonce length */ + NONCE_LENGTH +
           sizeof(uint32_t) + key_material_length() + sizeof(uint32_t) /* tag length */ +
           TAG_LENGTH + enforced_.SerializedSize() + unenforced_.SerializedSize();
}

const uint8_t BLOB_VERSION = 0;

uint8_t* KeyBlob::Serialize(uint8_t* buf, const uint8_t* end) const {
    const uint8_t* start __attribute__((__unused__)) = buf;
    *buf++ = BLOB_VERSION;
    buf = append_size_and_data_to_buf(buf, end, nonce(), NONCE_LENGTH);
    buf = append_size_and_data_to_buf(buf, end, encrypted_key_material(), key_material_length());
    buf = append_size_and_data_to_buf(buf, end, tag(), TAG_LENGTH);
    buf = enforced_.Serialize(buf, end);
    buf = unenforced_.Serialize(buf, end);
    assert(buf - start == static_cast<ptrdiff_t>(SerializedSize()));
    return buf;
}

bool KeyBlob::Deserialize(const uint8_t** buf_ptr, const uint8_t* end) {
    const uint8_t* start = *buf_ptr;
    uint8_t version = *(*buf_ptr)++;
    size_t nonce_length;
    size_t tag_length;
    if (version != BLOB_VERSION ||
        !copy_size_and_data_from_buf(buf_ptr, end, &nonce_length, &nonce_) ||
        nonce_length != NONCE_LENGTH ||
        !copy_size_and_data_from_buf(buf_ptr, end, &key_material_length_,
                                     &encrypted_key_material_) ||
        !copy_size_and_data_from_buf(buf_ptr, end, &tag_length, &tag_) ||
        tag_length != TAG_LENGTH || !enforced_.Deserialize(buf_ptr, end) ||
        !unenforced_.Deserialize(buf_ptr, end)) {
        *buf_ptr = start;
        // This blob failed to parse.  Either it's corrupted or it's a blob generated by an earlier
        // version of keymaster using a previous blob format which did not include the version byte
        // or the nonce or tag length fields.  So we try to parse it as that previous version.
        //
        // Note that it's not really a problem if we erronously parse a corrupted blob, because
        // decryption will fail the authentication check.
        //
        // A bigger potential problem is: What if a valid unversioned blob appears to parse
        // correctly as a versioned blob?  It would then be rejected during decryption, causing a
        // valid key to become unusable.  If this is a disk encryption key, upgrading to a keymaster
        // version with the new format would destroy the user's data.
        //
        // What is the probability that an unversioned key could be successfully parsed as a version
        // 0 key?  The first 12 bytes of an unversioned key are the nonce, which, in the only
        // keymaster version released with unversioned keys, is chosen randomly.  In order for an
        // unversioned key to parse as a version 0 key, the following must be true about the first
        // five of those random bytes:
        //
        // 1.  The first byte must be zero.  This will happen with probability 1/2^8.
        //
        // 2.  The second through fifth bytes must contain an unsigned integer value equal to
        //     NONCE_LENGTH.  This will happen with probability 1/2^32.
        //
        // Based on those two checks alone, the probability of interpreting an unversioned blob as a
        // version 0 blob is 1/2^40.  That's small enough to be negligible, but there are additional
        // checks which lower it further.
        *buf_ptr = start;
        if (!DeserializeUnversionedBlob(buf_ptr, end))
            return false;
    }
    return ExtractKeyCharacteristics();
}

bool KeyBlob::DeserializeUnversionedBlob(const uint8_t** buf_ptr, const uint8_t* end) {
    nonce_.reset(new uint8_t[NONCE_LENGTH]);
    tag_.reset(new uint8_t[TAG_LENGTH]);
    if (!nonce_.get() || !tag_.get()) {
        error_ = KM_ERROR_MEMORY_ALLOCATION_FAILED;
        return false;
    }

    if (!copy_from_buf(buf_ptr, end, nonce_.get(), NONCE_LENGTH) ||
        !copy_size_and_data_from_buf(buf_ptr, end, &key_material_length_,
                                     &encrypted_key_material_) ||
        !copy_from_buf(buf_ptr, end, tag_.get(), TAG_LENGTH) ||
        !enforced_.Deserialize(buf_ptr, end) || !unenforced_.Deserialize(buf_ptr, end)) {
        encrypted_key_material_.reset();
        error_ = KM_ERROR_INVALID_KEY_BLOB;
        return false;
    }
    return ExtractKeyCharacteristics();
}

KeyBlob::KeyBlob(const AuthorizationSet& enforced, const AuthorizationSet& unenforced)
    : error_(KM_ERROR_OK), enforced_(enforced), unenforced_(unenforced) {
}

void KeyBlob::SetEncryptedKey(uint8_t* encrypted_key_material, size_t encrypted_key_material_length,
                              uint8_t* nonce, uint8_t* tag) {
    ClearKeyData();
    encrypted_key_material_.reset(encrypted_key_material);
    key_material_length_ = encrypted_key_material_length;
    nonce_.reset(nonce);
    tag_.reset(tag);
}

bool KeyBlob::ExtractKeyCharacteristics() {
    if (!enforced_.GetTagValue(TAG_ALGORITHM, &algorithm_) &&
        !unenforced_.GetTagValue(TAG_ALGORITHM, &algorithm_)) {
        error_ = KM_ERROR_UNSUPPORTED_ALGORITHM;
        return false;
    }
    if (!enforced_.GetTagValue(TAG_KEY_SIZE, &key_size_bits_) &&
        !unenforced_.GetTagValue(TAG_KEY_SIZE, &key_size_bits_)) {
        error_ = KM_ERROR_UNSUPPORTED_KEY_SIZE;
        return false;
    }
    return true;
}

}  // namespace keymaster
