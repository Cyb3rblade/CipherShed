/*
 Copyright (c) 2008 TrueCrypt Foundation. All rights reserved.

 Governed by the TrueCrypt License 2.4 the full text of which is contained
 in the file License.txt included in TrueCrypt binary and source code
 distribution packages.
*/

#include "Platform/Time.h"
#include "Crc32.h"
#include "EncryptionModeXTS.h"
#include "Pkcs5Kdf.h"
#include "Pkcs5Kdf.h"
#include "VolumeHeader.h"
#include "VolumeException.h"

namespace TrueCrypt
{
	VolumeHeader::VolumeHeader (uint32 size)
		: HeaderSize (size), EncryptedHeaderDataSize (size - EncryptedHeaderDataOffset)
	{
	}

	VolumeHeader::~VolumeHeader ()
	{
		VolumeKeyAreaCrc32 = 0;
		VolumeCreationTime = 0;
		HeaderCreationTime = 0;
		mVolumeType = VolumeType::Unknown;
		HiddenVolumeDataSize = 0;
		VolumeDataSize = 0;
		EncryptedAreaStart = 0;
		EncryptedAreaLength = 0;
	}

	void VolumeHeader::Create (const BufferPtr &headerBuffer, VolumeHeaderCreationOptions &options)
	{
		if (options.DataKey.Size() != options.EA->GetKeySize() * 2 || options.Salt.Size() != GetSaltSize())
			throw ParameterIncorrect (SRC_POS);

		headerBuffer.Zero();

		HeaderVersion = CurrentHeaderVersion;
		RequiredMinProgramVersion = CurrentRequiredMinProgramVersion;

		DataAreaKey.Zero();
		DataAreaKey.CopyFrom (options.DataKey);

		VolumeCreationTime = Time::GetCurrent();
		HiddenVolumeDataSize = (options.Type == VolumeType::Hidden ? options.VolumeSize : 0);
		VolumeDataSize = options.VolumeSize;

		EncryptedAreaStart = 0;
		EncryptedAreaLength = options.VolumeSize;

		EA = options.EA;
		shared_ptr <EncryptionMode> mode (new EncryptionModeXTS ());
		EA->SetMode (mode);

		EncryptNew (headerBuffer, options.Salt, options.HeaderKey, options.Kdf);
	}

	bool VolumeHeader::Decrypt (const ConstBufferPtr &encryptedData, const VolumePassword &password, const EncryptionAlgorithmList &encryptionAlgorithms, const EncryptionModeList &encryptionModes)
	{
		if (password.Size() < 1)
			throw PasswordEmpty (SRC_POS);

		ConstBufferPtr salt (encryptedData.GetRange (SaltOffset, SaltSize));
		SecureBuffer header (EncryptedHeaderDataSize);
		SecureBuffer headerKey (GetLargestSerializedKeySize());

		foreach (shared_ptr <Pkcs5Kdf> pkcs5, Pkcs5Kdf::GetAvailableAlgorithms())
		{
			pkcs5->DeriveKey (headerKey, password, salt);

			foreach (shared_ptr <EncryptionMode> mode, encryptionModes)
			{
				if (typeid (*mode) != typeid (EncryptionModeXTS))
					mode->SetKey (headerKey.GetRange (0, mode->GetKeySize()));

				foreach (shared_ptr <EncryptionAlgorithm> ea, encryptionAlgorithms)
				{
					if (!ea->IsModeSupported (mode))
						continue;

					if (typeid (*mode) == typeid (EncryptionModeXTS))
					{
						ea->SetKey (headerKey.GetRange (0, ea->GetKeySize()));
						
						mode = mode->GetNew();
						mode->SetKey (headerKey.GetRange (ea->GetKeySize(), ea->GetKeySize()));
					}
					else
					{
						ea->SetKey (headerKey.GetRange (LegacyEncryptionModeKeyAreaSize, ea->GetKeySize()));
					}

					ea->SetMode (mode);

					header.CopyFrom (encryptedData.GetRange (EncryptedHeaderDataOffset, EncryptedHeaderDataSize));
					ea->Decrypt (header);

					if (Deserialize (header, ea, mode))
					{
						EA = ea;
						Pkcs5 = pkcs5;
						return true;
					}
				}
			}
		}

		return false;
	}

	bool VolumeHeader::Deserialize (const ConstBufferPtr &header, shared_ptr <EncryptionAlgorithm> &ea, shared_ptr <EncryptionMode> &mode)
	{
		if (header.Size() != EncryptedHeaderDataSize)
			throw ParameterIncorrect (SRC_POS);

		if (header[0] != 'T' ||
			header[1] != 'R' ||
			header[2] != 'U' ||
			header[3] != 'E')
			return false;

		size_t offset = 4;
		HeaderVersion =	DeserializeEntry <uint16> (header, offset);

		if (HeaderVersion < MinAllowedHeaderVersion)
			return false;

		RequiredMinProgramVersion = DeserializeEntry <uint16> (header, offset);
		
		if (RequiredMinProgramVersion > Version::Number())
			throw HigherVersionRequired (SRC_POS);

		VolumeKeyAreaCrc32 = DeserializeEntry <uint32> (header, offset);
		VolumeCreationTime = DeserializeEntry <uint64> (header, offset);
		HeaderCreationTime = DeserializeEntry <uint64> (header, offset);
		HiddenVolumeDataSize = DeserializeEntry <uint64> (header, offset);
		mVolumeType = (HiddenVolumeDataSize != 0 ? VolumeType::Hidden : VolumeType::Normal);
		VolumeDataSize = DeserializeEntry <uint64> (header, offset);
		EncryptedAreaStart = DeserializeEntry <uint64> (header, offset);
		EncryptedAreaLength = DeserializeEntry <uint64> (header, offset);

		offset = DataAreaKeyOffset;

		if (VolumeKeyAreaCrc32 != Crc32::ProcessBuffer (header.GetRange (offset, DataKeyAreaMaxSize)))
			return false;

		DataAreaKey.CopyFrom (header.GetRange (offset, DataKeyAreaMaxSize));
		
		ea = ea->GetNew();
		mode = mode->GetNew();
		
		if (typeid (*mode) == typeid (EncryptionModeXTS))
		{
			ea->SetKey (header.GetRange (offset, ea->GetKeySize()));
			mode->SetKey (header.GetRange (offset + ea->GetKeySize(), ea->GetKeySize()));
		}
		else
		{
			mode->SetKey (header.GetRange (offset, mode->GetKeySize()));
			ea->SetKey (header.GetRange (offset + LegacyEncryptionModeKeyAreaSize, ea->GetKeySize()));
		}

		ea->SetMode (mode);

		return true;
	}

	template <typename T>
	T VolumeHeader::DeserializeEntry (const ConstBufferPtr &header, size_t &offset) const
	{
		offset += sizeof (T);

		if (offset > header.Size())
			throw ParameterIncorrect (SRC_POS);

		return Endian::Big (*reinterpret_cast<const T *> (header.Get() + offset - sizeof (T)));
	}

	void VolumeHeader::EncryptNew (const BufferPtr &newHeaderBuffer, const ConstBufferPtr &newSalt, const ConstBufferPtr &newHeaderKey, shared_ptr <Pkcs5Kdf> newPkcs5Kdf)
	{
		if (newHeaderBuffer.Size() != HeaderSize || newSalt.Size() != SaltSize)
			throw ParameterIncorrect (SRC_POS);

		shared_ptr <EncryptionMode> mode = EA->GetMode()->GetNew();
		shared_ptr <EncryptionAlgorithm> ea = EA->GetNew();

		if (typeid (*mode) == typeid (EncryptionModeXTS))
		{
			mode->SetKey (newHeaderKey.GetRange (EA->GetKeySize(), EA->GetKeySize()));
			ea->SetKey (newHeaderKey.GetRange (0, ea->GetKeySize()));
		}
		else
		{
			mode->SetKey (newHeaderKey.GetRange (0, mode->GetKeySize()));
			ea->SetKey (newHeaderKey.GetRange (LegacyEncryptionModeKeyAreaSize, ea->GetKeySize()));
		}

		ea->SetMode (mode);

		BufferPtr headerData = newHeaderBuffer.GetRange (EncryptedHeaderDataOffset, EncryptedHeaderDataSize);

		Serialize (headerData);
		ea->Encrypt (headerData);

		Memory::Copy (newHeaderBuffer, newSalt, newSalt.Size());

		if (newPkcs5Kdf)
			Pkcs5 = newPkcs5Kdf;
	}

	size_t VolumeHeader::GetLargestSerializedKeySize ()
	{
		size_t largestKey = EncryptionAlgorithm::GetLargestKeySize (EncryptionAlgorithm::GetAvailableAlgorithms());
		
		// XTS mode requires the same key size as the encryption algorithm.
		// Legacy modes may require larger key than XTS.
		if (LegacyEncryptionModeKeyAreaSize + largestKey > largestKey * 2)
			return LegacyEncryptionModeKeyAreaSize + largestKey;

		return largestKey * 2;
	}

	void VolumeHeader::Serialize (const BufferPtr &header) const
	{
		if (header.Size() != EncryptedHeaderDataSize)
			throw ParameterIncorrect (SRC_POS);

		header.Zero();

		header[0] = 'T';
		header[1] = 'R';
		header[2] = 'U';
		header[3] = 'E';
		size_t offset = 4;

		header.GetRange (DataAreaKeyOffset, DataAreaKey.Size()).CopyFrom (DataAreaKey);

		SerializeEntry (HeaderVersion, header, offset);
		SerializeEntry (RequiredMinProgramVersion, header, offset);
		SerializeEntry (Crc32::ProcessBuffer (header.GetRange (DataAreaKeyOffset, DataKeyAreaMaxSize)), header, offset);
		SerializeEntry (VolumeCreationTime, header, offset);
		SerializeEntry (Time::GetCurrent(), header, offset);
		SerializeEntry (HiddenVolumeDataSize, header, offset);
		SerializeEntry (VolumeDataSize, header, offset);
		SerializeEntry (EncryptedAreaStart, header, offset);
		SerializeEntry (EncryptedAreaLength, header, offset);

	}

	template <typename T>
	void VolumeHeader::SerializeEntry (const T &entry, const BufferPtr &header, size_t &offset) const
	{
		offset += sizeof (T);

		if (offset > header.Size())
			throw ParameterIncorrect (SRC_POS);

		*reinterpret_cast<T *> (header.Get() + offset - sizeof (T)) = Endian::Big (entry);
	}
}