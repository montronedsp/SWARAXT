// Copyright 2009 Emilie Gillet.
// Portable host variant for SwaraXt — buffer storage sized explicitly for MSVC.
// Derived from pichenettes/avril ring_buffer.h @ af7266e.

#ifndef AVRLIB_RING_BUFFER_H_
#define AVRLIB_RING_BUFFER_H_

#include "avrlib/avrlib.h"
#include "avrlib/base.h"

namespace avrlib {

template<typename Owner>
class RingBuffer : public Input, Output {
 public:
    typedef typename Owner::Value Value;
    enum {
        size = Owner::buffer_size,
        data_size = Owner::data_size
    };

    RingBuffer() {}

    inline uint8_t capacity() const { return size; }
    inline void Write(Value v)
    {
        // Host: never spin-wait on a buffer that nothing drains.
        if (writable())
            Overwrite(v);
    }
    inline uint8_t writable() const
    {
        return static_cast<uint8_t>((read_ptr_ - write_ptr_ - 1) & (size - 1));
    }
    inline uint8_t NonBlockingWrite(Value v)
    {
        if (writable())
        {
            Overwrite(v);
            return 1;
        }
        return 0;
    }
    inline void Overwrite(Value v)
    {
        const uint8_t w = write_ptr_;
        buffer_[w] = v;
        write_ptr_ = static_cast<uint8_t>((w + 1) & (size - 1));
    }

    inline uint8_t Requested() const { return 0; }
    inline Value Read()
    {
        if (! readable())
            return Value();
        return ImmediateRead();
    }
    inline uint8_t readable() const
    {
        return static_cast<uint8_t>((write_ptr_ - read_ptr_) & (size - 1));
    }
    inline int16_t NonBlockingRead()
    {
        return readable() ? ImmediateRead() : -1;
    }
    inline Value ImmediateRead()
    {
        const uint8_t r = read_ptr_;
        const Value result = buffer_[r];
        read_ptr_ = static_cast<uint8_t>((r + 1) & (size - 1));
        return result;
    }
    inline void Flush() { write_ptr_ = read_ptr_; }

 private:
    Value buffer_[size] {};
    volatile uint8_t read_ptr_ = 0;
    volatile uint8_t write_ptr_ = 0;

    DISALLOW_COPY_AND_ASSIGN(RingBuffer);
};

}  // namespace avrlib

#endif  // AVRLIB_RING_BUFFER_H_
