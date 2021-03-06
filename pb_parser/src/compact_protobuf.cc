/*
 * =====================================================================================
 *
 *       Filename:  compact_protobuf.cc
 *        Created:  06/09/14 14:17:04
 *         Author:  peng wang
 *          Email:  pw2191195@gmail.com
 *    Description:  
 *
 * =====================================================================================
 */

#include "compact_protobuf.h"
#include "protobuf_parser.h"
#include "protobuf_helper.h"
#include "protobuf_encoder.h"

#include <cstdio>
#include <boost/foreach.hpp>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/wire_format.h>

namespace CompactProtobuf
{
    /*-----------------------------------------------------------------------------
     *  Field
     *-----------------------------------------------------------------------------*/
    int Field::create_times = 0;
    Field::Field()
    {
        ++Field::create_times;
        values.reserve(1);
    }

    bool Field::has_value() const
    {
        return not values.empty();
    }

    Value * Field::value()
    {
        assert (values.size() == 1);
        return &(*values.begin());
    }

    Value * Field::value(size_t idx)
    {
        assert (values.size() > idx);
        return &values[idx];
    }

    const Value * Field::value(size_t idx) const
    {
        assert (values.size() > idx);
        return &values[idx];
    }

    ValuePtr Field::Delete(size_t idx)
    {
        assert (values.size() > idx);
        ValueList::iterator iter = values.begin();
        std::advance(iter, idx);
        ValuePtr v(new Value);
        *v = *iter;
        values.erase(iter);
        return v;
    }

    void Field::FastAppend(const Value& value)
    {
        Value * v = new Value;
        v->encoded = value.encoded;
        if (wire_type == kLengthDelimited)
        {
            v->decoded.primitive.len = value.decoded.primitive.len;
        }
        Append(v);
    }

    void Field::Append(Value * value)
    {
        values.push_back(value);
    }

    Value::Value()
    {
        ++Value::create_times;
    }

    Value::Value(const Value& rhs)
    {
        encoded = rhs.encoded;
        decoded = rhs.decoded;
        ++Value::copied_times;
    }

    int Value::copied_times = 0;
    int Value::create_times = 0;
    /*-----------------------------------------------------------------------------
     *  Environment
     *-----------------------------------------------------------------------------*/
    Environment::Environment()
    {
    }

    Environment::~Environment()
    {
    }

    bool Environment::Register(const Slice& slice)
    {
        if (slice.end <= slice.start) return false;
        FileDescriptorSet protos;
        bool ok = protos.ParseFromArray(slice.start, slice.end - slice.start);
        if (ok)
        {
            for (int i = 0; i != protos.file_size(); ++i)
            {
                ok = pool_.BuildFile(protos.file(i));
                if (not ok) return false;
            }
            return true;
        }
        return false;
    }

    const Descriptor* Environment::FindMessageTypeByName(const string& name) const
    {
        return pool_.FindMessageTypeByName(name);
    }

    Message::Message(const Descriptor* descriptor)
        :descriptor_(descriptor)
    {
    }

    Message::~Message()
    {
    }

    bool Message::Init(const Slice& slice)
    {
        bool ok = Helper::ParseMessageLazy(slice, descriptor_, &fields_);
        if (not ok) return false;

        vector<int> unknown_keys;
        for (FieldMap::const_iterator iter = fields_.begin(); iter != fields_.end(); ++iter)
        {
            if (not iter->second->unknown) continue;

            if (not unknown_fields_) unknown_fields_.reset(new FieldMap);

            unknown_keys.push_back (iter->first);
        }

        if (unknown_fields_)
        {
            BOOST_FOREACH (int key, unknown_keys)
            {
                unknown_fields_->transfer (fields_.find(key), fields_);
            }
        }
        return true;
    }

    bool Message::FromString(const string& encoded)
    {
        Slice slice;
        slice.start = reinterpret_cast<Byte*>(const_cast<char*>(encoded.data()));
        slice.end = slice.start + encoded.size();
        return Init(slice);
    }

    bool Message::has_field() const
    {
        return fields_.size() != 0u;
    }

    bool Message::has_field(const string& name) const
    {
        const FieldDescriptor* field_descriptor = descriptor_->FindFieldByName(name);
        if (field_descriptor)
        {
            return fields_.find(field_descriptor->number()) != fields_.end();
        }
        return false;
    }

    bool Message::has_unknown_field() const
    {
        return unknown_fields_ != NULL and unknown_fields_->size() != 0u;
    }

    void Message::Clear()
    {
        /* clear everything except unknwon fields in this message and all embedded messages*/
        vector<int> left_keys;
        for (FieldMap::iterator iter = fields_.begin(); iter != fields_.end(); ++iter)
        {
            Field * field = iter->second;
            if (field->field_descriptor->type() == FieldDescriptor::TYPE_MESSAGE)
            {
                vector<MessagePtr> messages;
                for (size_t idx = 0; idx != field->values.size(); ++idx)
                {
                    Value & value = field->values[idx];
                    MessagePtr & message = value.decoded.m;
                    message->Clear();

                    if (message->has_field() or message->has_unknown_field())
                    {
                        /* save this value */
                        messages.push_back(message);
                    }
                }
                if (not messages.empty())
                {
                    field->values.clear();
                    for (size_t idx = 0; idx != messages.size(); ++idx)
                    {
                        Value * v = new Value;
                        v->decoded.m = messages[idx];
                        field->Append (v);
                    }
                    left_keys.push_back (iter->first);
                }
            }
        }
        FieldMap left_fields;
        BOOST_FOREACH (int key, left_keys)
        {
            bool ok = left_fields.transfer (fields_.find(key), fields_);
            (void)ok;
            assert (ok);
        }
        fields_.swap (left_fields);
    }

    uint32_t Message::GetInteger(const string& name, size_t idx, uint32_t * hi)
    {
        const FieldDescriptor* field_descriptor = CheckInteger(name);
        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())              /* field no found */
        {
            if (field_descriptor->is_repeated()) assert (false); /* cannot index a empty repeated field with idx*/
            uint64_t default_value = Helper::DefaultIntegerValue(field_descriptor);
            return Helper::DecodeUInt64(default_value, hi);
        }

        Field & field = *iter->second;

        CheckValidIndex(field, field_descriptor, idx);
        TryDecodeField(&field, field_descriptor);

        assert (field.decoded);
        if (not field_descriptor->is_repeated()) idx = 0;
        uint64_t val = Helper::RetrieveIntegerValue(field, idx);
        return Helper::DecodeUInt64(val, hi);
    }

    string Message::GetString(const string& name, size_t idx)
    {
        const FieldDescriptor* field_descriptor = CheckString(name);

        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())
        {
            return field_descriptor->default_value_string();
        }

        Field & field = *iter->second;
        CheckValidIndex(field, field_descriptor, idx);
        TryDecodeField(&field, field_descriptor);

        assert (field.decoded);
        if (field_descriptor->is_repeated() == false) idx = 0;
        return field.value(idx)->decoded.s;
    }

    double Message::GetReal(const string& name, size_t idx)
    {
        const FieldDescriptor* field_descriptor = CheckReal(name);

        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())
        {
            return Helper::DefaultRealValue(field_descriptor);
        }

        Field & field = *iter->second;
        CheckValidIndex(field, field_descriptor, idx);
        TryDecodeField(&field, field_descriptor);

        assert (field.decoded);
        if (field_descriptor->is_repeated() == false) idx = 0;
        return Helper::RetrieveRealValue(field, idx);
    }

    size_t Message::GetFieldSize(const string& name)
    {
        const FieldDescriptor* field_descriptor = descriptor_->FindFieldByName(name);
        assert (field_descriptor);

        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end()) return 0;
        else
        {
            TryDecodeField(iter->second, field_descriptor);
            return iter->second->values.size();
        }
    }

    void Message::SetInteger(const string& name, size_t idx, uint32_t low, uint32_t hi)
    {
        const FieldDescriptor* field_descriptor = CheckInteger(name);
        uint64_t val = hi;
        val <<= 32;
        val |= low;

        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())              /* field no found */
        {
            Field * field = AddKnownField(field_id, field_descriptor);
            Value * v = new Value;
            Helper::SetInteger(v, field_descriptor->type(), val);
            field->Append(v);
        }
        else
        {
            Field * field = iter->second;

            CheckValidIndex(*field, field_descriptor, idx);
            TryDecodeField(field, field_descriptor);

            assert (field->decoded);
            if (not field_descriptor->is_repeated()) idx = 0;
            Helper::SetInteger(field->value(idx), field_descriptor->type(), val);
        }
    }

    void Message::SetReal(const string& name, size_t idx, double val)
    {
        const FieldDescriptor* field_descriptor = CheckReal(name);
        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())              /* field no found */
        {
            Field * field = AddKnownField(field_id, field_descriptor);
            Value * v = new Value;
            Helper::SetReal(v, field_descriptor->type(), val);
            field->Append(v);
        }
        else
        {
            Field * field = iter->second;

            CheckValidIndex(*field, field_descriptor, idx);
            TryDecodeField(field, field_descriptor);

            assert (field->decoded);
            if (not field_descriptor->is_repeated()) idx = 0;
            Helper::SetReal(field->value(idx), field_descriptor->type(), val);
        }
    }

    void Message::SetString(const string& name, size_t idx, const string& val)
    {
        const FieldDescriptor* field_descriptor = CheckString(name);
        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())              /* field no found */
        {
            Field * field = AddKnownField(field_id, field_descriptor);
            Value * v = new Value;
            Helper::SetString(v, field_descriptor->type(), val);
            field->Append(v);
        }
        else
        {
            Field * field = iter->second;

            CheckValidIndex(*field, field_descriptor, idx);
            TryDecodeField(field, field_descriptor);

            assert (field->decoded);
            if (not field_descriptor->is_repeated()) idx = 0;
            field->value(idx)->decoded.s = val;
        }

    }

    Message * Message::GetMessage(const string& name, size_t idx)
    {
        const FieldDescriptor* field_descriptor = CheckMessage(name);

        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end()) return NULL;

        Field & field = *iter->second;
        CheckValidIndex(field, field_descriptor, idx);
        TryDecodeField(&field, field_descriptor);

        assert (field.decoded);
        if (not field_descriptor->is_repeated()) idx = 0;
        return field.value(idx)->decoded.m.get();
    }

    void Message::AddInteger(const string& name, uint32_t low, uint32_t hi)
    {
        const FieldDescriptor* field_descriptor = CheckInteger(name);
        uint64_t val = hi;
        val <<= 32;
        val |= low;

        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())              /* field no found */
        {
            Field * field = AddKnownField(field_id, field_descriptor);
            Value * v = new Value;
            Helper::SetInteger(v, field_descriptor->type(), val);
            field->Append(v);
        }
        else if (not field_descriptor->is_repeated())
        {
            /* cannot add value of non-repeated fields, use SetInteger*/
            assert (false);
        }
        else
        {
            Field * field = iter->second;
            TryDecodeField(field, field_descriptor);

            assert (field->decoded);
            Value * v = new Value;
            Helper::SetInteger(v, field_descriptor->type(), val);
            field->Append(v);
        }
    }

    void Message::AddReal(const string& name, double val)
    {
        const FieldDescriptor* field_descriptor = CheckReal(name);

        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())              /* field no found */
        {
            Field * field = AddKnownField(field_id, field_descriptor);
            Value * v = new Value;
            Helper::SetReal(v, field_descriptor->type(), val);
            field->Append(v);
        }
        else if (not field_descriptor->is_repeated())
        {
            /* cannot add value of non-repeated fields, use SetInteger*/
            assert (false);
        }
        else
        {
            Field * field = iter->second;
            TryDecodeField(field, field_descriptor);

            assert (field->decoded);
            Value * v = new Value;
            Helper::SetReal(v, field_descriptor->type(), val);
            field->Append(v);
        }
    }

    void Message::AddString(const string& name, const string& val)
    {
        const FieldDescriptor* field_descriptor = CheckString(name);

        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())              /* field no found */
        {
            Field * field = AddKnownField(field_id, field_descriptor);
            Value * v = new Value;
            Helper::SetString(v, field_descriptor->type(), val);
            field->Append(v);
        }
        else if (not field_descriptor->is_repeated())
        {
            /* cannot add value of non-repeated fields, use SetInteger*/
            assert (false);
        }
        else
        {
            Field * field = iter->second;
            TryDecodeField(field, field_descriptor);

            assert (field->decoded);
            Value * v = new Value;
            Helper::SetString(v, field_descriptor->type(), val);
            field->Append(v);
        }
    }

    Message * Message::AddMessage(const string& name)
    {
        const FieldDescriptor* field_descriptor = CheckMessage(name);

        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        if (iter == fields_.end())
        {
            Field * field = AddKnownField(field_id, field_descriptor);
            return InternalAddMessage(field, field_descriptor);
        }

        if (not field_descriptor->is_repeated())
        {
            assert (false);                     /* use GetMessage */
        }

        Field * field = iter->second;
        TryDecodeField(field, field_descriptor);

        assert (field->decoded);
        return InternalAddMessage(field, field_descriptor);
    }

    uint32_t Message::DeleteInteger(const string& name, size_t idx, uint32_t * hi)
    {
        const FieldDescriptor* field_descriptor = CheckInteger(name);
        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        assert (iter != fields_.end());         /* field not found */

        Field * field = iter->second;
        CheckValidIndex(*field, field_descriptor, idx);
        TryDecodeField(field, field_descriptor);

        assert (field->decoded);
        if (not field_descriptor->is_repeated()) idx = 0;
        uint64_t val = Helper::RetrieveIntegerValue(*field, idx);
        field->Delete(idx);
        if (not field->has_value()) fields_.erase( fields_.find(field->id) );
        return Helper::DecodeUInt64(val, hi);
    }

    double Message::DeleteReal(const string& name, size_t idx)
    {
        const FieldDescriptor* field_descriptor = CheckReal(name);
        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        assert (iter != fields_.end());         /* field not found */

        Field * field = iter->second;
        CheckValidIndex(*field, field_descriptor, idx);
        TryDecodeField(field, field_descriptor);

        assert (field->decoded);
        if (not field_descriptor->is_repeated()) idx = 0;
        double val = field->Delete(idx)->decoded.primitive.d.d;
        if (not field->has_value()) fields_.erase( fields_.find(field->id) );
        return val;
    }

    string Message::DeleteString(const string& name, size_t idx)
    {
        const FieldDescriptor* field_descriptor = CheckString(name);
        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        assert (iter != fields_.end());         /* field not found */

        Field * field = iter->second;
        CheckValidIndex(*field, field_descriptor, idx);
        TryDecodeField(field, field_descriptor);

        assert (field->decoded);
        if (not field_descriptor->is_repeated()) idx = 0;
        string val = field->Delete(idx)->decoded.s;
        if (not field->has_value()) fields_.erase( fields_.find(field->id) );
        return val;
    }

    MessagePtr Message::DeleteMessage(const string& name, size_t idx)
    {
        const FieldDescriptor* field_descriptor = CheckMessage(name);
        int field_id = field_descriptor->number();
        FieldMap::iterator iter = fields_.find(field_id);
        assert (iter != fields_.end());         /* field not found */

        Field * field = iter->second;
        CheckValidIndex(*field, field_descriptor, idx);
        TryDecodeField(field, field_descriptor);

        assert (field->decoded);
        if (not field_descriptor->is_repeated()) idx = 0;
        MessagePtr val = field->Delete(idx)->decoded.m;
        if (not field->has_value()) fields_.erase( fields_.find(field->id) );
        return val;
    }

    bool Message::ToString(std::string* output)
    {
        Encoder::EncoderBuffer buffer;
        bool bok = Encoder::EncodeMessage(this, &buffer);
        if (not bok) return false;

        buffer.ToString(output);
        return true;
    }

    Field * Message::AddKnownField(int id, const FieldDescriptor* field_descriptor)
    {
        Field * field = new Field;
        field->decoded = true;
        field->unknown = false;
        field->id = id;
        field->wire_type = Helper::GetWireType(field_descriptor);
        field->field_descriptor = field_descriptor;
        fields_.insert (id, field);
        return field;
    }

    void Message::CheckValidIndex(const Field& field, const FieldDescriptor* field_descriptor, size_t idx)
    {
        if (field_descriptor->is_repeated() and idx >= field.values.size())
        {
            /* out of range */
            assert (false);
        }
    }

    void Message::TryDecodeField(Field* field, const FieldDescriptor* field_descriptor)
    {
        if (not field->decoded and not Helper::DecodeField(field))
        {
            assert (false);                     /* invalid data */
        }
    }

    const FieldDescriptor* Message::CheckInteger(const string& name) const
    {
        const FieldDescriptor* field_descriptor = descriptor_->FindFieldByName(name);
        assert (field_descriptor);
        FieldDescriptor::CppType cpp_type = field_descriptor->cpp_type();
        assert (cpp_type == FieldDescriptor::CPPTYPE_INT32 
                or cpp_type == FieldDescriptor::CPPTYPE_INT64
                or cpp_type == FieldDescriptor::CPPTYPE_UINT32
                or cpp_type == FieldDescriptor::CPPTYPE_UINT64
                or cpp_type == FieldDescriptor::CPPTYPE_BOOL
                or cpp_type == FieldDescriptor::CPPTYPE_ENUM);
        return field_descriptor;
    }

    const FieldDescriptor* Message::CheckString(const string& name) const
    {
        const FieldDescriptor* field_descriptor = descriptor_->FindFieldByName(name);
        assert (field_descriptor);
        FieldDescriptor::CppType cpp_type = field_descriptor->cpp_type();
        assert (cpp_type == FieldDescriptor::CPPTYPE_STRING);
        return field_descriptor;
    }

    const FieldDescriptor* Message::CheckReal(const string& name) const
    {
        const FieldDescriptor* field_descriptor = descriptor_->FindFieldByName(name);
        assert (field_descriptor);
        FieldDescriptor::CppType cpp_type = field_descriptor->cpp_type();
        assert (cpp_type == FieldDescriptor::CPPTYPE_DOUBLE
                or cpp_type == FieldDescriptor::CPPTYPE_FLOAT);
        return field_descriptor;
    }

    const FieldDescriptor* Message::CheckMessage(const string& name) const
    {
        const FieldDescriptor* field_descriptor = descriptor_->FindFieldByName(name);
        assert (field_descriptor);
        FieldDescriptor::CppType cpp_type = field_descriptor->cpp_type();
        assert (cpp_type == FieldDescriptor::CPPTYPE_MESSAGE);
        return field_descriptor;
    }

    Message * Message::InternalAddMessage(Field* field, const FieldDescriptor* field_descriptor)
    {
        MessagePtr embedded_message = Helper::MakeMessage(field_descriptor);

        Value * v = new Value;
        v->decoded.m = embedded_message;
        field->Append(v);
        return embedded_message.get();
    }
}
