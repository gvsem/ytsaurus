package compression

// CodecID is a type that holds all available codec types.
type CodecID int8

const (
	CodecIDNone CodecID = 0

	CodecIDSnappy CodecID = 1

	CodecIDLz4                CodecID = 4
	CodecIDLz4HighCompression CodecID = 5

	CodecIDZlib1 CodecID = 19
	CodecIDZlib2 CodecID = 20
	CodecIDZlib3 CodecID = 21
	CodecIDZlib4 CodecID = 22
	CodecIDZlib5 CodecID = 23
	CodecIDZlib6 CodecID = 2
	CodecIDZlib7 CodecID = 24
	CodecIDZlib8 CodecID = 25
	CodecIDZlib9 CodecID = 3

	CodecIDZstd1 CodecID = 26
	CodecIDZstd3 CodecID = 28
	CodecIDZstd7 CodecID = 32
)

func (i CodecID) String() string {
	switch i {
	case CodecIDNone:
		return "none"

	case CodecIDSnappy:
		return "snappy"

	case CodecIDLz4:
		return "lz4"
	case CodecIDLz4HighCompression:
		return "lz4_high_compression"

	case CodecIDZlib1:
		return "zlib_1"
	case CodecIDZlib2:
		return "zlib_2"
	case CodecIDZlib3:
		return "zlib_3"
	case CodecIDZlib4:
		return "zlib_4"
	case CodecIDZlib5:
		return "zlib_5"
	case CodecIDZlib6:
		return "zlib_6"
	case CodecIDZlib7:
		return "zlib_7"
	case CodecIDZlib8:
		return "zlib_8"
	case CodecIDZlib9:
		return "zlib_9"

	case CodecIDZstd1:
		return "zstd_1"
	case CodecIDZstd3:
		return "zstd_3"
	case CodecIDZstd7:
		return "zstd_7"
	}
	return ""
}

// Codec is a generic interface for compression/decompression.
type Codec interface {
	// Compress compresses given block.
	Compress(block []byte) ([]byte, error)
	// Decompress decompresses given block.
	Decompress(block []byte) ([]byte, error)
	// GetID returns codec identifier.
	GetID() CodecID
}

// NewCodec creates codec by id.
func NewCodec(id CodecID) Codec {
	switch id {
	case CodecIDSnappy:
		return &CodecSnappy{}

	case CodecIDLz4:
		return &CodecLz4{}
	case CodecIDLz4HighCompression:
		return &CodecLz4HighCompression{}

	case CodecIDZlib1:
		return CodecZlib(1)
	case CodecIDZlib2:
		return CodecZlib(2)
	case CodecIDZlib3:
		return CodecZlib(3)
	case CodecIDZlib4:
		return CodecZlib(4)
	case CodecIDZlib5:
		return CodecZlib(5)
	case CodecIDZlib6:
		return CodecZlib(6)
	case CodecIDZlib7:
		return CodecZlib(7)
	case CodecIDZlib8:
		return CodecZlib(8)
	case CodecIDZlib9:
		return CodecZlib(9)

	case CodecIDZstd1:
		return CodecZstd(1)
	case CodecIDZstd3:
		return CodecZstd(3)
	case CodecIDZstd7:
		return CodecZstd(7)

	default:
		return CodecNone{}
	}
}

// CodecNone is a special codec that does not any compression.
type CodecNone struct{}

// Compress returns block as is.
func (c CodecNone) Compress(block []byte) ([]byte, error) {
	return block, nil
}

// Decompress returns block as is.
func (c CodecNone) Decompress(block []byte) ([]byte, error) {
	return block, nil
}

func (c CodecNone) GetID() CodecID {
	return CodecIDNone
}
