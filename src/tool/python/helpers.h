#pragma once

namespace xlang
{
    inline auto get_start_time()
    {
        return std::chrono::high_resolution_clock::now();
    }

    inline auto get_elapsed_time(std::chrono::time_point<std::chrono::high_resolution_clock> const& start)
    {
        return std::chrono::duration_cast<std::chrono::duration<int64_t, std::milli>>(std::chrono::high_resolution_clock::now() - start).count();
    }

    struct separator
    {
        writer& w;
        bool first{ true };

        void operator()()
        {
            if (first)
            {
                first = false;
            }
            else
            {
                w.write(", ");
            }
        }
    };

    bool is_flags_enum(TypeDef const& type);

    template <typename T>
    struct signature_handler_base
    {
        void handle_class(TypeDef const& /*type*/) { throw_invalid("handle_class not implemented"); }
        void handle_delegate(TypeDef const& /*type*/) { throw_invalid("handle_delegate not implemented"); }
        void handle_guid(TypeRef const& /*type*/) { throw_invalid("handle_guid not implemented"); }
        void handle_interface(TypeDef const& /*type*/) { throw_invalid("handle_interface not implemented"); }
        void handle_struct(TypeDef const& /*type*/) { throw_invalid("handle_struct not implemented"); }

        void handle_enum(TypeDef const& type)
        {
            if (is_flags_enum(type))
            {
                static_cast<T*>(this)->handle(ElementType::U4);
            }
            else
            {
                static_cast<T*>(this)->handle(ElementType::I4);
            }
        }

        void handle(TypeRef const& type)
        {
            auto ns = type.TypeNamespace();
            auto name = type.TypeName();

            if (name == "Guid" && ns == "System")
            {
                static_cast<T*>(this)->handle_guid(type);
            }
            else
            {
                static_cast<T*>(this)->handle(find_required(type));
            }
        }

        void handle(TypeDef const& type)
        {
            switch (get_category(type))
            {
            case category::class_type:
                static_cast<T*>(this)->handle_class(type);
                break;
            case category::delegate_type:
                static_cast<T*>(this)->handle_delegate(type);
                break;
            case category::interface_type:
                static_cast<T*>(this)->handle_interface(type);
                break;
            case category::enum_type:
                static_cast<T*>(this)->handle_enum(type);
                break;
            case category::struct_type:
                static_cast<T*>(this)->handle_struct(type);
                break;
            }
        }

        void handle(coded_index<TypeDefOrRef> const& type)
        {
            switch (type.type())
            {
            case TypeDefOrRef::TypeDef:
                static_cast<T*>(this)->handle(type.TypeDef());
                break;

            case TypeDefOrRef::TypeRef:
                static_cast<T*>(this)->handle(type.TypeRef());
                break;

            case TypeDefOrRef::TypeSpec:
                static_cast<T*>(this)->handle(type.TypeSpec().Signature().GenericTypeInst());
                break;
            }
        }

        void handle_start_generic() { throw_invalid("handle_start_generic not implemented"); }

        void handle_end_generic() { throw_invalid("handle_end_generic not implemented"); }

        void handle(GenericTypeInstSig const& type)
        {
            handle(type.GenericType());
            static_cast<T*>(this)->handle_start_generic();
            for (auto&& arg : type.GenericArgs())
            {
                handle(arg);
            }
            static_cast<T*>(this)->handle_end_generic();
        }

        void handle(ElementType /*type*/) { throw_invalid("handle(ElementType) not implemented"); }

        void handle(GenericTypeIndex /*var*/) { throw_invalid("handle(GenericTypeIndex) not implemented"); }

        void handle(TypeSig const& signature)
        {
            visit(signature.Type(),
                [&](auto&& type)
            {
                static_cast<T*>(this)->handle(type);
            });
        }
    };

    struct method_signature
    {
        using param_t = std::pair<Param, ParamSig const*>;

        explicit method_signature(MethodDef const& method) :
            m_method(method.Signature())
        {
            auto params = method.ParamList();

            if (m_method.ReturnType() && params.first != params.second && params.first.Sequence() == 0)
            {
                m_return = params.first;
                ++params.first;
            }

            for (uint32_t i{}; i != m_method.Params().size(); ++i)
            {
                m_params.emplace_back(params.first + i, m_method.Params().data() + i);
            }
        }

        std::vector<param_t>& params()
        {
            return m_params;
        }

        std::vector<param_t> const& params() const
        {
            return m_params;
        }

        auto const& return_signature() const
        {
            return m_method.ReturnType();
        }

        auto return_param_name() const
        {
            std::string_view name;

            if (m_return)
            {
                name = m_return.Name();
            }
            else
            {
                name = "winrt_impl_return_value";
            }

            return name;
        }

        bool has_params() const
        {
            return !m_params.empty();
        }

    private:

        MethodDefSig m_method;
        std::vector<param_t> m_params;
        Param m_return;
    };

    bool is_exclusive_to(TypeDef const& type)
    {
        return get_category(type) == category::interface_type && get_attribute(type, "Windows.Foundation.Metadata", "ExclusiveToAttribute");
    }

    bool is_flags_enum(TypeDef const& type)
    {
        return get_category(type) == category::enum_type && get_attribute(type, "System", "FlagsAttribute");
    }

    bool is_ptype(TypeDef const& type)
    {
        return distance(type.GenericParam()) > 0;
    }

    struct interface_info
    {
        coded_index<TypeDefOrRef> type;
        std::pair<MethodDef, MethodDef> methods;
    };

    inline void get_interfaces_impl(writer& w, std::map<std::string, interface_info>& result, std::pair<InterfaceImpl, InterfaceImpl>&& children)
    {
        for (auto&& impl : children)
        {
            interface_info info{ impl.Interface() };
            auto name = w.write_temp("%", info.type);

            {
                // This is for correctness rather than an optimization (but helps performance as well).
                // If the interface was not previously inserted, carry on and recursively insert it.
                // If a previous insertion was defaulted we're done as it is correctly captured.
                // If a newly discovered instance of a previous insertion is not defaulted, we're also done.
                // If it was previously captured as non-defaulted but now found as defaulted, we carry on and
                // rediscover it as we need it to be defaulted recursively.

                auto found = result.find(name);

                if (found != result.end())
                {
                    continue;
                }
            }

            TypeDef definition;
            writer::generic_param_guard guard;

            switch (info.type.type())
            {
            case TypeDefOrRef::TypeDef:
            {
                definition = info.type.TypeDef();
                break;
            }
            case TypeDefOrRef::TypeRef:
            {
                definition = find_required(info.type.TypeRef());
                break;
            }
            case TypeDefOrRef::TypeSpec:
            {
                auto type_signature = info.type.TypeSpec().Signature();
                guard = w.push_generic_params(type_signature.GenericTypeInst());
                auto signature = type_signature.GenericTypeInst();
                definition = find_required(signature.GenericType().TypeRef());
                break;
            }
            }

            info.methods = definition.MethodList();
            get_interfaces_impl(w, result, definition.InterfaceImpl());
            result[name] = std::move(info);
        }
    };

    inline auto get_interfaces(writer& w, TypeDef const& type)
    {
        std::map<std::string, interface_info> result;
        get_interfaces_impl(w, result, type.InterfaceImpl());

        return result;
    }

    inline bool is_constructor(MethodDef const& method)
    {
        return method.Flags().RTSpecialName() && method.Name() == ".ctor";
    }

    inline auto get_methods2(writer& w, TypeDef const& type)
    {
        std::map<std::string_view, std::vector<std::pair<coded_index<TypeDefOrRef>, MethodDef>>> methods{};

        for (auto&& method : type.MethodList())
        {
            if (is_constructor(method))
            {
                continue;
            }

            methods[method.Name()].emplace_back(type.coded_index<TypeDefOrRef>(), method);
        }

        if (get_category(type) == category::interface_type)
        {
            for (auto&& info : get_interfaces(w, type))
            {
                for (auto&& method : info.second.methods)
                {
                    methods[method.Name()].emplace_back(info.second.type, method);
                }
            }
        }

#ifdef XLANG_DEBUG
        // ensure all the overloads of a given method match the static flag of the first method
        for (auto&& method : methods)
        {
            auto const& overloads = method.second;
            if (overloads.size() > 1)
            {
                auto static_method = overloads[0].second.Flags().Static();

                XLANG_ASSERT(std::all_of(overloads.begin(), overloads.end(),
                    [static_method](auto const& m) { return m.second.Flags().Static() == static_method; }));
            }
        }
#endif

        return std::move(methods);
    }

    inline std::vector<MethodDef> get_methods(writer& w, TypeDef const& type)
    {
        std::vector<MethodDef> methods{};

        for (auto&& method : type.MethodList())
        {
            if (is_constructor(method))
            {
                continue;
            }

            methods.push_back(method);
        }

        auto name = type.TypeName();

        if (get_category(type) == category::interface_type)
        {
            for (auto&& info : get_interfaces(w, type))
            {
                for (auto&& method : info.second.methods)
                {
                    methods.push_back(method);
                }
            }
        }

        return std::move(methods);
    }

    inline bool is_get_method(MethodDef const& method)
    {
        return method.SpecialName() && starts_with(method.Name(), "get_");
    }

    inline bool is_put_method(MethodDef const& method)
    {
        return method.SpecialName() && starts_with(method.Name(), "put_");
    }

    inline bool is_add_method(MethodDef const& method)
    {
        return method.SpecialName() && starts_with(method.Name(), "add_");
    }

    inline bool is_remove_method(MethodDef const& method)
    {
        return method.SpecialName() && starts_with(method.Name(), "remove_");
    }

    struct property_type
    {
        MethodDef get;
        MethodDef set;
    };

    property_type get_property_methods(Property const& prop)
    {
        MethodDef get_method{}, set_method{};

        for (auto&& method_semantic : prop.MethodSemantic())
        {
            auto semantic = method_semantic.Semantic();

            if (semantic.Getter())
            {
                get_method = method_semantic.Method();
            }
            else if (semantic.Setter())
            {
                set_method = method_semantic.Method();
            }
            else
            {
                throw_invalid("Properties can only have get and set methods");
            }
        }

        XLANG_ASSERT(get_method);

        if (set_method)
        {
            XLANG_ASSERT(get_method.Flags().Static() == set_method.Flags().Static());
        }

        return { get_method, set_method };
    }

    struct event_type
    {
        MethodDef add;
        MethodDef remove;
    };

    event_type get_event_methods(Event const& evt)
    {
        MethodDef add_method{}, remove_method{};

        for (auto&& method_semantic : evt.MethodSemantic())
        {
            auto semantic = method_semantic.Semantic();

            if (semantic.AddOn())
            {
                add_method = method_semantic.Method();
            }
            else if (semantic.RemoveOn())
            {
                remove_method = method_semantic.Method();
            }
            else
            {
                throw_invalid("Events can only have add and remove methods");
            }
        }

        XLANG_ASSERT(add_method);
        XLANG_ASSERT(remove_method);
        XLANG_ASSERT(add_method.Flags().Static() == remove_method.Flags().Static());

        return { add_method, remove_method };
    }

    bool has_dealloc(TypeDef const& type)
    {
        auto category = get_category(type);
        return category == category::interface_type || (category == category::class_type && !type.Flags().Abstract());
    }

    enum class param_category
    {
        in,
        out,
        pass_array,
        fill_array,
        receive_array,
    };

    auto get_param_category(method_signature::param_t const& param)
    {
        if (param.second->Type().is_szarray())
        {
            if (param.first.Flags().In())
            {
                return param_category::pass_array;
            }
            else if (param.second->ByRef())
            {
                XLANG_ASSERT(param.first.Flags().Out());
                return param_category::fill_array;
            }
            else
            {
                XLANG_ASSERT(param.first.Flags().Out());
                return param_category::receive_array;
            }
        }
        else
        {
            if (param.first.Flags().In())
            {
                XLANG_ASSERT(!param.first.Flags().Out());
                return param_category::in;
            }
            else
            {
                XLANG_ASSERT(param.first.Flags().Out());
                return param_category::out;
            }
        }
    }

    bool is_in_param(method_signature::param_t const& param)
    {
        auto category = get_param_category(param);

        if (category == param_category::fill_array)
        {
            throw_invalid("fill aray param not impl");
        }

        return (category == param_category::in || category == param_category::pass_array);
    }

    int count_in_param(std::vector<method_signature::param_t> const& params)
    {
        int count{ 0 };

        for (auto&& param : params)
        {
            if (is_in_param(param))
            {
                count++;
            }
        }

        return count;
    }

    int count_out_param(std::vector<method_signature::param_t> const& params)
    {
        int count{ 0 };

        for (auto&& param : params)
        {
            if (!is_in_param(param))
            {
                count++;
            }
        }

        return count;
    }
}