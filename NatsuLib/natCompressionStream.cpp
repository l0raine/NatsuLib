#include "stdafx.h"
#include "natCompressionStream.h"
#include <zlib.h>
#include <zutil.h>

using namespace NatsuLib;

namespace NatsuLib
{
	namespace detail_
	{
		// 无线程安全保证
		struct DeflateStreamImpl
		{
			enum
			{
				DefaultWindowBits = -15,	// 使用负数以略过头部
			};

			DeflateStreamImpl(int level, int method, int windowBits, int memLevel, int strategy)
				: ZStream{}, Compress{ true }, InputBufferLeft{}, OutputBufferLeft{}
			{
				const auto ret = deflateInit2(&ZStream, level, method, windowBits, memLevel, strategy);
				if (ret != Z_OK)
				{
					nat_Throw(natErrException, NatErr_InternalErr, "deflateInit2 failed with code {0}(description: {1})."_nv, ret, U8StringView{ ZStream.msg });
				}
			}

			explicit DeflateStreamImpl(int windowBits)
				: ZStream{}, Compress{ false }, InputBufferLeft{}, OutputBufferLeft{}
			{
				const auto ret = inflateInit2(&ZStream, windowBits);
				if (ret != Z_OK)
				{
					nat_Throw(natErrException, NatErr_InternalErr, "inflateInit2 failed with code {0}(description: {1})."_nv, ret, U8StringView{ ZStream.msg });
				}
			}

			~DeflateStreamImpl()
			{
				if (Compress)
				{
					deflateEnd(&ZStream);
				}
				else
				{
					inflateEnd(&ZStream);
				}
			}

			void SetInput(ncData inputBuffer, size_t bufferLength) noexcept
			{
				assert(ZStream.avail_in == 0 && "Some data left in previous input.");

				constexpr auto max = std::numeric_limits<uInt>::max();
				// 假定nByte与Bytef相同
				ZStream.next_in = const_cast<z_const Bytef*>(inputBuffer);
				if (bufferLength > static_cast<size_t>(max))
				{
					ZStream.avail_in = max;
					InputBufferLeft = static_cast<size_t>(bufferLength - static_cast<size_t>(max));
				}
				else
				{
					ZStream.avail_in = static_cast<uInt>(bufferLength);
					InputBufferLeft = 0;
				}
			}

			void SetOutput(nData outputBuffer, size_t bufferLength) noexcept
			{
				constexpr auto max = std::numeric_limits<uInt>::max();
				// 假定nByte与Bytef相同
				ZStream.next_out = outputBuffer;
				if (bufferLength > static_cast<size_t>(max))
				{
					ZStream.avail_out = max;
					OutputBufferLeft = static_cast<size_t>(bufferLength - static_cast<size_t>(max));
				}
				else
				{
					ZStream.avail_out = static_cast<uInt>(bufferLength);
					OutputBufferLeft = 0;
				}
			}

			int DoNext() noexcept
			{
				constexpr auto max = std::numeric_limits<uInt>::max();
				if (ZStream.avail_in == 0)
				{
					ZStream.avail_in = InputBufferLeft > static_cast<size_t>(max) ? max : static_cast<uInt>(InputBufferLeft);
					InputBufferLeft -= ZStream.avail_in;
				}
				if (ZStream.avail_out == 0)
				{
					ZStream.avail_out = OutputBufferLeft > static_cast<size_t>(max) ? max : static_cast<uInt>(OutputBufferLeft);
					OutputBufferLeft -= ZStream.avail_out;
				}

				const auto flush = InputBufferLeft ? Z_NO_FLUSH : Z_FINISH;
				return Compress ? deflate(&ZStream, flush) : inflate(&ZStream, flush);
			}

			int Flush() noexcept
			{
				constexpr auto max = std::numeric_limits<uInt>::max();
				if (ZStream.avail_in == 0)
				{
					ZStream.avail_in = InputBufferLeft > static_cast<size_t>(max) ? max : static_cast<uInt>(InputBufferLeft);
					InputBufferLeft -= ZStream.avail_in;
				}
				if (ZStream.avail_out == 0)
				{
					ZStream.avail_out = OutputBufferLeft > static_cast<size_t>(max) ? max : static_cast<uInt>(OutputBufferLeft);
					OutputBufferLeft -= ZStream.avail_out;
				}

				return Compress ? deflate(&ZStream, Z_SYNC_FLUSH) : inflate(&ZStream, Z_SYNC_FLUSH);
			}

			z_stream ZStream;
			const nBool Compress;
			size_t InputBufferLeft, OutputBufferLeft;
		};
	}
}

natDeflateStream::natDeflateStream(natRefPointer<natStream> stream)
	: m_InternalStream{ std::move(stream) }, m_Buffer{}, m_Impl{ std::make_unique<detail_::DeflateStreamImpl>(detail_::DeflateStreamImpl::DefaultWindowBits) }, m_WroteData{ false }
{
	if (!m_InternalStream->CanRead())
	{
		nat_Throw(natErrException, NatErr_InvalidArg, "stream should be readable."_nv);
	}
}

natDeflateStream::natDeflateStream(natRefPointer<natStream> stream, CompressionLevel compressionLevel)
	: m_InternalStream{ std::move(stream) }, m_Buffer{}, m_WroteData{ false }
{
	if (!m_InternalStream)
	{
		nat_Throw(natErrException, NatErr_InvalidArg, "stream should be a valid pointer."_nv);
	}

	if (!m_InternalStream->CanWrite())
	{
		nat_Throw(natErrException, NatErr_InvalidArg, "stream should be writable."_nv);
	}
	
	switch (compressionLevel)
	{
	case CompressionLevel::Optimal:
		m_Impl = std::make_unique<detail_::DeflateStreamImpl>(Z_BEST_COMPRESSION, Z_DEFLATED, detail_::DeflateStreamImpl::DefaultWindowBits, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY);
		break;
	case CompressionLevel::Fastest:
		m_Impl = std::make_unique<detail_::DeflateStreamImpl>(Z_BEST_SPEED, Z_DEFLATED, detail_::DeflateStreamImpl::DefaultWindowBits, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY);
		break;
	case CompressionLevel::NoCompression:
		m_Impl = std::make_unique<detail_::DeflateStreamImpl>(Z_NO_COMPRESSION, Z_DEFLATED, detail_::DeflateStreamImpl::DefaultWindowBits, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY);
		break;
	default:
		assert(!"Invalid compressionLevel.");
	}

	m_Impl->SetOutput(m_Buffer, sizeof m_Buffer);
}

natDeflateStream::~natDeflateStream()
{
}

natRefPointer<natStream> natDeflateStream::GetUnderlyingStream() const noexcept
{
	return m_InternalStream;
}

nBool natDeflateStream::CanWrite() const
{
	return m_Impl->Compress && m_InternalStream->CanWrite();
}

nBool natDeflateStream::CanRead() const
{
	return !m_Impl->Compress && m_InternalStream->CanRead();
}

nBool natDeflateStream::CanResize() const
{
	return false;
}

nBool natDeflateStream::CanSeek() const
{
	return false;
}

nBool natDeflateStream::IsEndOfStream() const
{
	return m_InternalStream->IsEndOfStream();
}

nLen natDeflateStream::GetSize() const
{
	nat_Throw(natErrException, NatErr_NotSupport, "This type of stream does not support GetSize."_nv);
}

void natDeflateStream::SetSize(nLen)
{
	nat_Throw(natErrException, NatErr_NotSupport, "This type of stream does not support SetSize."_nv);
}

nLen natDeflateStream::GetPosition() const
{
	nat_Throw(natErrException, NatErr_NotSupport, "This type of stream does not support GetPosition."_nv);
}

void natDeflateStream::SetPosition(NatSeek, nLong)
{
	nat_Throw(natErrException, NatErr_NotSupport, "This type of stream does not support SetPosition."_nv);
}

nLen natDeflateStream::ReadBytes(nData pData, nLen Length)
{
	if (!CanRead())
	{
		nat_Throw(natErrException, NatErr_IllegalState, "Stream is not readable."_nv);
	}

	auto pRead = pData;
	auto dataRemain = Length;

	while (true)
	{
		// 输出之前可能未输出的内容
		m_Impl->SetOutput(pRead, dataRemain);
		const auto ret = m_Impl->DoNext();	// 忽略此处可能的部分错误
		if (ret == Z_DATA_ERROR)
		{
			nat_Throw(InvalidData, "Invalid data with zlib message ({0})."_nv, U8StringView{ m_Impl->ZStream.msg });
		}
		const auto currentReadBytes = Length - m_Impl->OutputBufferLeft - m_Impl->ZStream.avail_out;
		assert(dataRemain >= currentReadBytes);
		pRead += currentReadBytes;
		dataRemain -= currentReadBytes;

		if (dataRemain == 0)
		{
			break;
		}

		if (ret == Z_STREAM_END && m_Impl->ZStream.avail_in == 0 && m_Impl->ZStream.avail_out == 0)
		{
			break;
		}

		if (m_InternalStream->IsEndOfStream())
		{
			break;
		}
		const auto readBytes = m_InternalStream->ReadBytes(m_Buffer, sizeof m_Buffer);
		if (readBytes == 0)
		{
			break;
		}
		
		assert(readBytes <= sizeof m_Buffer);

		// 检查了输入已经全部处理完毕了吗？
		m_Impl->SetInput(m_Buffer, readBytes);
	}

	return Length - dataRemain;
}

nLen natDeflateStream::WriteBytes(ncData pData, nLen Length)
{
	if (!CanWrite())
	{
		nat_Throw(natErrException, NatErr_IllegalState, "Stream is not writable."_nv);
	}

	auto writtenBytes = writeAll();
	m_Impl->SetInput(pData, Length);
	writtenBytes += writeAll();
	m_WroteData = true;
	return writtenBytes;
}

void natDeflateStream::Flush()
{
	if (m_Impl->Compress && m_WroteData)
	{
		writeAll();

		while (true)
		{
			m_Impl->SetOutput(m_Buffer, sizeof m_Buffer);
			const auto ret = m_Impl->Flush();
			if (ret == Z_OK)
			{
				m_InternalStream->WriteBytes(m_Buffer, sizeof m_Buffer - m_Impl->OutputBufferLeft - m_Impl->ZStream.avail_out);
			}
			else
			{
				break; // 忽略错误
			}
		}
	}
}

nLen natDeflateStream::writeAll()
{
	assert(CanWrite());

	nLen totalWrittenBytes{};
	int ret;
	while (m_Impl->ZStream.avail_in != 0)
	{
		do
		{
			ret = m_Impl->DoNext();

			if (ret == Z_DATA_ERROR)
			{
				nat_Throw(InvalidData, "Invalid data with zlib message ({0})."_nv, U8StringView{ m_Impl->ZStream.msg });
			}

			const auto availableDataSize = DefaultBufferSize - m_Impl->OutputBufferLeft - m_Impl->ZStream.avail_out;
			const auto currentWrittenBytes = m_InternalStream->WriteBytes(m_Buffer, availableDataSize);
			// 是否需要检查？
			if (currentWrittenBytes < availableDataSize)
			{
				nat_Throw(natErrException, NatErr_InternalErr, "Partial data written({0}/{1} requested)."_nv, currentWrittenBytes, availableDataSize);
			}
			totalWrittenBytes += currentWrittenBytes;

			if (ret == Z_BUF_ERROR)
			{
				m_Impl->SetOutput(m_Buffer, sizeof m_Buffer);
			}
		} while (ret != Z_OK && ret != Z_STREAM_END);
	}
	
	return totalWrittenBytes;
}

natCrc32Stream::natCrc32Stream(natRefPointer<natStream> stream)
	: m_InternalStream{ std::move(stream) }, m_Crc32{}, m_CurrentPosition{}
{
	if (!m_InternalStream)
	{
		nat_Throw(natErrException, NatErr_InvalidArg, "stream should not be nullptr."_nv);
	}
	if (!m_InternalStream->CanWrite())
	{
		nat_Throw(natErrException, NatErr_InvalidArg, "stream should be writable."_nv);
	}
}

natCrc32Stream::~natCrc32Stream()
{
}

natRefPointer<natStream> natCrc32Stream::GetUnderlyingStream() const noexcept
{
	return m_InternalStream;
}

nuInt natCrc32Stream::GetCrc32() const noexcept
{
	return m_Crc32;
}

nBool natCrc32Stream::CanWrite() const
{
	return true;
}

nBool natCrc32Stream::CanRead() const
{
	return false;
}

nBool natCrc32Stream::CanResize() const
{
	return false;
}

nBool natCrc32Stream::CanSeek() const
{
	return false;
}

nBool natCrc32Stream::IsEndOfStream() const
{
	return m_InternalStream->IsEndOfStream();
}

nLen natCrc32Stream::GetSize() const
{
	nat_Throw(natErrException, NatErr_NotSupport, "The type of this stream does not support this operation."_nv);
}

void natCrc32Stream::SetSize(nLen)
{
	nat_Throw(natErrException, NatErr_NotSupport, "The type of this stream does not support this operation."_nv);
}

nLen natCrc32Stream::GetPosition() const
{
	return m_CurrentPosition;
}

void natCrc32Stream::SetPosition(NatSeek, nLong)
{
	nat_Throw(natErrException, NatErr_NotSupport, "The type of this stream does not support this operation."_nv);
}

nLen natCrc32Stream::ReadBytes(nData /*pData*/, nLen /*Length*/)
{
	nat_Throw(natErrException, NatErr_NotSupport, "The type of this stream does not support this operation."_nv);
}

nLen natCrc32Stream::WriteBytes(ncData pData, nLen Length)
{
	if (!Length)
	{
		return 0;
	}

	m_Crc32 = static_cast<nuInt>(crc32_z(m_Crc32, pData, static_cast<z_size_t>(Length)));
	const auto writtenBytes = m_InternalStream->WriteBytes(pData, Length);
	m_CurrentPosition += writtenBytes;
	return writtenBytes;
}

void natCrc32Stream::Flush()
{
	m_InternalStream->Flush();
}
