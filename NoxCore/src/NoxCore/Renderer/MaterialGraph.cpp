#include "NoxCore/Renderer/MaterialGraph.h"

#include <algorithm>
#include <format>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "NoxCore/NodeGraph/GraphCompiler.h"
#include "NoxCore/NodeGraph/NodeType.h"

namespace Nox
{
    namespace
    {
        // ---- Values: Slang expressions with their screen derivatives -------------------------------------------------------------

        // A value in the generated code: an expression of `Components` floats and, when it varies across the screen in a way a
        // texture lookup cares about (UVs, positions), the expressions of its derivatives along x and y (empty: zero).
        struct Value
        {
            std::string Code;
            std::string Dx, Dy;
            int Components = 1;
            bool HasDerivatives() const { return !Dx.empty(); }
        };

        std::string TypeName(int components)
        {
            return components == 1 ? "float" : "float" + std::to_string(components);
        }

        std::string Zero(int components)
        {
            return components == 1 ? "0.0" : TypeName(components) + "(0.0)";
        }

        std::string Float(float value)
        {
            std::string text = std::format("{}", value);
            if (text.find_first_of(".eEn") == std::string::npos) // ("1" -> "1.0": a float literal; "inf" / "nan" never get here)
                text += ".0";
            return text;
        }

        Value Constant(std::string code, int components)
        {
            return Value{ std::move(code), {}, {}, components };
        }

        // To `components` floats (Unreal's rules): a float is spread to every component, a vector cut to its first ones or padded with 0.
        Value Convert(const Value& value, int components)
        {
            if (value.Components == components)
                return value;
            auto convert = [&](const std::string& code) -> std::string
            {
                if (code.empty())
                    return code;
                if (value.Components == 1)
                    return TypeName(components) + "(" + code + ")";
                if (value.Components > components)
                    return "(" + code + ")." + std::string("xyzw").substr(0, components);
                std::string padded = TypeName(components) + "(" + code;
                for (int i = value.Components; i < components; ++i)
                    padded += ", 0.0";
                return padded + ")";
            };
            return Value{ convert(value.Code), convert(value.Dx), convert(value.Dy), components };
        }

        // ---- The generated function ---------------------------------------------------------------------------------------------

        struct Context
        {
            std::string Body;
            uint32_t Temporaries = 0;
            std::vector<MaterialGraphParameter> Parameters;
            uint32_t ParameterSlots = 0;
            uint32_t TextureSlots = 0;
            uint32_t Outputs = 0;
            std::string Error;

            // A value into named locals (its derivatives too), so what reads it twice computes it once.
            Value Store(const Value& value)
            {
                const std::string name = "t" + std::to_string(Temporaries++);
                const std::string type = TypeName(value.Components);
                Body += "    const " + type + " " + name + " = " + value.Code + ";\n";
                Value stored{ name, {}, {}, value.Components };
                if (value.HasDerivatives())
                {
                    Body += "    const " + type + " " + name + "_dx = " + value.Dx + ";\n";
                    Body += "    const " + type + " " + name + "_dy = " + value.Dy + ";\n";
                    stored.Dx = name + "_dx";
                    stored.Dy = name + "_dy";
                }
                return stored;
            }

            // A parameter's slot: the same name reads the same slot (a second node with it shares the first's default).
            const MaterialGraphParameter* Parameter(const std::string& name, MaterialGraphParameter::Kind type, const glm::vec4& value, uint64_t texture)
            {
                for (const MaterialGraphParameter& parameter : Parameters)
                {
                    if (parameter.Name != name)
                        continue;
                    if ((parameter.Type == MaterialGraphParameter::Kind::Texture) != (type == MaterialGraphParameter::Kind::Texture))
                    {
                        Error = std::format("the parameter '{}' is both a texture and a number", name);
                        return nullptr;
                    }
                    return &parameter;
                }
                MaterialGraphParameter parameter;
                parameter.Name = name;
                parameter.Type = type;
                parameter.Default = value;
                parameter.DefaultTexture = texture;
                if (type == MaterialGraphParameter::Kind::Texture)
                {
                    if (TextureSlots >= MaterialGraphMaxTextures)
                    {
                        Error = std::format("more than {} textures in one material graph", MaterialGraphMaxTextures);
                        return nullptr;
                    }
                    parameter.Slot = TextureSlots++;
                }
                else
                {
                    if (ParameterSlots >= MaterialGraphMaxParameters)
                    {
                        Error = std::format("more than {} scalar and vector parameters in one material graph", MaterialGraphMaxParameters);
                        return nullptr;
                    }
                    parameter.Slot = ParameterSlots++;
                }
                Parameters.push_back(parameter);
                return &Parameters.back();
            }
        };

        // Two values combined component-wise (Unreal's rule: equal sizes, or one of them a float). `derivative(a, ad, b, bd)` gives one
        // derivative from the operands and theirs (empty: no derivatives).
        bool Combine(Context& context, const Value& a, const Value& b, const char* what, Value& out, const std::function<std::string(const std::string&, const std::string&)>& code,
                     const std::function<std::string(const std::string&, const std::string&, const std::string&, const std::string&)>& derivative)
        {
            if (a.Components != b.Components && a.Components != 1 && b.Components != 1)
            {
                context.Error = std::format("{} of a {} and a {}: connect a Component Mask or Append to make them the same size", what,
                                            TypeName(a.Components), TypeName(b.Components));
                return false;
            }
            const int components = std::max(a.Components, b.Components);
            const Value x = Convert(a, components), y = Convert(b, components);
            out = Value{ code(x.Code, y.Code), {}, {}, components };
            if ((x.HasDerivatives() || y.HasDerivatives()) && derivative)
            {
                const std::string zero = Zero(components);
                out.Dx = derivative(x.Code, x.HasDerivatives() ? x.Dx : zero, y.Code, y.HasDerivatives() ? y.Dx : zero);
                out.Dy = derivative(x.Code, x.HasDerivatives() ? x.Dy : zero, y.Code, y.HasDerivatives() ? y.Dy : zero);
            }
            return true;
        }

        // A function of one value; `derivative(x, dx)` its derivative by the chain rule (null: none -- a step).
        Value Unary(const Value& x, const std::function<std::string(const std::string&)>& code,
                    const std::function<std::string(const std::string&, const std::string&)>& derivative, int components = 0)
        {
            Value out{ code(x.Code), {}, {}, components > 0 ? components : x.Components };
            if (x.HasDerivatives() && derivative)
            {
                out.Dx = derivative(x.Code, x.Dx);
                out.Dy = derivative(x.Code, x.Dy);
            }
            return out;
        }

        // ---- Node types -----------------------------------------------------------------------------------------------------------

        struct EmitArgs
        {
            Context& Ctx;
            const CompiledNode& Node;
            uint32_t NodeId;
            std::vector<Value>& In;  // every input pin's value (a literal or a link), Convert-ible
            std::vector<bool>& Connected;
            std::vector<Value>& Out;
        };
        using EmitFn = std::function<bool(EmitArgs&)>;

        std::unordered_map<std::string, EmitFn>& Emitters()
        {
            static std::unordered_map<std::string, EmitFn> emitters;
            return emitters;
        }

        NodePinDesc V(const std::string& name) { return { name, "Value" }; }

        void Register(const std::string& name, const std::string& category, std::vector<NodePinDesc> inputs, std::vector<NodePinDesc> outputs,
                      std::vector<std::pair<std::string, NodeGraphValue>> properties, EmitFn emit,
                      std::unordered_map<std::string, std::string> assetHints = {})
        {
            NodeTypeDesc desc;
            desc.TypeName = name;
            desc.Domain = kMaterialGraphDomain;
            desc.Category = category;
            desc.Inputs = std::move(inputs);
            desc.Outputs = std::move(outputs);
            desc.DefaultProperties = std::move(properties);
            desc.PropertyAssetTypeHints = std::move(assetHints);
            Emitters()[name] = std::move(emit);
            NodeTypeRegistry::Register(std::move(desc));
        }

        const NodeGraphValue* Property(const CompiledNode& node, const std::string& name)
        {
            const auto found = node.Properties.find(name);
            return found != node.Properties.end() ? &found->second : nullptr;
        }

        std::string StringProperty(const CompiledNode& node, const std::string& name)
        {
            const NodeGraphValue* value = Property(node, name);
            const std::string* text = value ? std::get_if<std::string>(value) : nullptr;
            return text ? *text : std::string();
        }

        // An unconnected input's value: its literal (a float, a float2 or a colour), else 0.
        Value Literal(const CompiledNode& node, const std::string& pin)
        {
            const NodeGraphValue* value = Property(node, pin);
            if (!value)
                return Constant("0.0", 1);
            if (const float* f = std::get_if<float>(value))
                return Constant(Float(*f), 1);
            if (const int32_t* i = std::get_if<int32_t>(value))
                return Constant(Float(static_cast<float>(*i)), 1);
            if (const bool* b = std::get_if<bool>(value))
                return Constant(*b ? "1.0" : "0.0", 1);
            if (const glm::vec2* v = std::get_if<glm::vec2>(value))
                return Constant(std::format("float2({}, {})", Float(v->x), Float(v->y)), 2);
            if (const glm::vec4* v = std::get_if<glm::vec4>(value))
                return Constant(std::format("float4({}, {}, {}, {})", Float(v->x), Float(v->y), Float(v->z), Float(v->w)), 4);
            return Constant("0.0", 1);
        }

        void RegisterArithmetic()
        {
            auto binary = [](const char* name, const char* symbol, auto derivative)
            {
                Register(name, "Math", { V("A"), V("B") }, { V("Result") }, { { "A", 0.0f }, { "B", name == std::string("Multiply") || name == std::string("Divide") ? 1.0f : 0.0f } },
                         [name, symbol, derivative](EmitArgs& args)
                         {
                             return Combine(args.Ctx, args.In[0], args.In[1], name, args.Out[0],
                                            [symbol](const std::string& a, const std::string& b) { return "(" + a + " " + symbol + " " + b + ")"; }, derivative);
                         });
            };
            binary("Add", "+", [](const std::string&, const std::string& ad, const std::string&, const std::string& bd) { return "(" + ad + " + " + bd + ")"; });
            binary("Subtract", "-", [](const std::string&, const std::string& ad, const std::string&, const std::string& bd) { return "(" + ad + " - " + bd + ")"; });
            binary("Multiply", "*", [](const std::string& a, const std::string& ad, const std::string& b, const std::string& bd)
                   { return "(" + ad + " * " + b + " + " + a + " * " + bd + ")"; });
            binary("Divide", "/", [](const std::string& a, const std::string& ad, const std::string& b, const std::string& bd)
                   { return "((" + ad + " * " + b + " - " + a + " * " + bd + ") / (" + b + " * " + b + "))"; });

            auto minMax = [](const char* name, const char* function, const char* compare)
            {
                Register(name, "Math", { V("A"), V("B") }, { V("Result") }, { { "A", 0.0f }, { "B", 0.0f } },
                         [name, function, compare](EmitArgs& args)
                         {
                             return Combine(args.Ctx, args.In[0], args.In[1], name, args.Out[0],
                                            [function](const std::string& a, const std::string& b) { return std::string(function) + "(" + a + ", " + b + ")"; },
                                            [compare](const std::string& a, const std::string& ad, const std::string& b, const std::string& bd)
                                            { return "(" + a + " " + compare + " " + b + " ? " + ad + " : " + bd + ")"; });
                         });
            };
            minMax("Min", "min", "<");
            minMax("Max", "max", ">");

            Register("Power", "Math", { V("Base"), V("Exponent") }, { V("Result") }, { { "Base", 0.0f }, { "Exponent", 2.0f } },
                     [](EmitArgs& args)
                     {
                         // (The exponent's own derivative is left out: it is a constant in nearly every graph.)
                         return Combine(args.Ctx, args.In[0], args.In[1], "Power", args.Out[0],
                                        [](const std::string& a, const std::string& b) { return "pow(max(" + a + ", 0.0), " + b + ")"; },
                                        [](const std::string& a, const std::string& ad, const std::string& b, const std::string&)
                                        { return "(" + b + " * pow(max(" + a + ", 1e-6), " + b + " - 1.0) * " + ad + ")"; });
                     });

            Register("Lerp", "Math", { V("A"), V("B"), V("Alpha") }, { V("Result") }, { { "A", 0.0f }, { "B", 1.0f }, { "Alpha", 0.5f } },
                     [](EmitArgs& args)
                     {
                         Value ab;
                         if (!Combine(args.Ctx, args.In[0], args.In[1], "Lerp", ab, [](const std::string& a, const std::string&) { return a; }, nullptr))
                             return false;
                         const int components = ab.Components;
                         const Value& t = args.In[2];
                         if (t.Components != 1 && t.Components != components)
                         {
                             args.Ctx.Error = std::format("Lerp's Alpha is a {} for a {}", TypeName(t.Components), TypeName(components));
                             return false;
                         }
                         const Value a = Convert(args.In[0], components), b = Convert(args.In[1], components), alpha = Convert(t, components);
                         Value out{ "lerp(" + a.Code + ", " + b.Code + ", " + alpha.Code + ")", {}, {}, components };
                         if (a.HasDerivatives() || b.HasDerivatives() || alpha.HasDerivatives())
                         {
                             const std::string zero = Zero(components);
                             auto d = [&](bool x)
                             {
                                 const std::string ad = a.HasDerivatives() ? (x ? a.Dx : a.Dy) : zero, bd = b.HasDerivatives() ? (x ? b.Dx : b.Dy) : zero;
                                 const std::string td = alpha.HasDerivatives() ? (x ? alpha.Dx : alpha.Dy) : zero;
                                 return "lerp(" + ad + ", " + bd + ", " + alpha.Code + ") + (" + b.Code + " - " + a.Code + ") * " + td;
                             };
                             out.Dx = d(true);
                             out.Dy = d(false);
                         }
                         args.Out[0] = out;
                         return true;
                     });

            Register("Clamp", "Math", { V("Value"), V("Min"), V("Max") }, { V("Result") }, { { "Value", 0.0f }, { "Min", 0.0f }, { "Max", 1.0f } },
                     [](EmitArgs& args)
                     {
                         const int components = args.In[0].Components;
                         const Value x = args.In[0], lo = Convert(args.In[1], components), hi = Convert(args.In[2], components);
                         args.Out[0] = Unary(x, [&](const std::string& v) { return "clamp(" + v + ", " + lo.Code + ", " + hi.Code + ")"; },
                                             [&](const std::string& v, const std::string& d)
                                             { return "(step(" + lo.Code + ", " + v + ") * step(" + v + ", " + hi.Code + ") * " + d + ")"; });
                         return true;
                     });

            // One-value functions with their chain rule (null: a step function, no derivative).
            auto unary = [](const char* name, std::function<std::string(const std::string&)> code,
                            std::function<std::string(const std::string&, const std::string&)> derivative)
            {
                Register(name, "Math", { V("Value") }, { V("Result") }, { { "Value", 0.0f } },
                         [code, derivative](EmitArgs& args)
                         {
                             args.Out[0] = Unary(args.In[0], code, derivative);
                             return true;
                         });
            };
            unary("One Minus", [](const std::string& v) { return "(1.0 - " + v + ")"; }, [](const std::string&, const std::string& d) { return "(-" + d + ")"; });
            unary("Saturate", [](const std::string& v) { return "saturate(" + v + ")"; },
                  [](const std::string& v, const std::string& d) { return "(step(0.0, " + v + ") * step(" + v + ", 1.0) * " + d + ")"; });
            unary("Abs", [](const std::string& v) { return "abs(" + v + ")"; }, [](const std::string& v, const std::string& d) { return "(sign(" + v + ") * " + d + ")"; });
            unary("Frac", [](const std::string& v) { return "frac(" + v + ")"; }, [](const std::string&, const std::string& d) { return d; });
            unary("Floor", [](const std::string& v) { return "floor(" + v + ")"; }, nullptr);
            unary("Ceil", [](const std::string& v) { return "ceil(" + v + ")"; }, nullptr);
            unary("Sine", [](const std::string& v) { return "sin(" + v + ")"; }, [](const std::string& v, const std::string& d) { return "(cos(" + v + ") * " + d + ")"; });
            unary("Cosine", [](const std::string& v) { return "cos(" + v + ")"; }, [](const std::string& v, const std::string& d) { return "(-sin(" + v + ") * " + d + ")"; });
            unary("Square Root", [](const std::string& v) { return "sqrt(max(" + v + ", 0.0))"; },
                  [](const std::string& v, const std::string& d) { return "(" + d + " / (2.0 * sqrt(max(" + v + ", 1e-8))))"; });
            unary("Normalize", [](const std::string& v) { return "normalize(" + v + ")"; }, nullptr);

            Register("Step", "Math", { V("Edge"), V("Value") }, { V("Result") }, { { "Edge", 0.5f }, { "Value", 0.0f } },
                     [](EmitArgs& args)
                     {
                         return Combine(args.Ctx, args.In[0], args.In[1], "Step", args.Out[0],
                                        [](const std::string& a, const std::string& b) { return "step(" + a + ", " + b + ")"; }, nullptr);
                     });
            Register("Smoothstep", "Math", { V("Min"), V("Max"), V("Value") }, { V("Result") }, { { "Min", 0.0f }, { "Max", 1.0f }, { "Value", 0.0f } },
                     [](EmitArgs& args)
                     {
                         const int components = args.In[2].Components;
                         const Value lo = Convert(args.In[0], components), hi = Convert(args.In[1], components);
                         args.Out[0] = Unary(args.In[2], [&](const std::string& v) { return "smoothstep(" + lo.Code + ", " + hi.Code + ", " + v + ")"; },
                                             [&](const std::string& v, const std::string& d)
                                             {
                                                 const std::string t = "saturate((" + v + " - " + lo.Code + ") / max(" + hi.Code + " - " + lo.Code + ", 1e-6))";
                                                 return "(6.0 * " + t + " * (1.0 - " + t + ") / max(" + hi.Code + " - " + lo.Code + ", 1e-6) * " + d + ")";
                                             });
                         return true;
                     });

            Register("Dot", "Vector", { V("A"), V("B") }, { V("Result") }, { { "A", 0.0f }, { "B", 0.0f } },
                     [](EmitArgs& args)
                     {
                         Value product;
                         if (!Combine(args.Ctx, args.In[0], args.In[1], "Dot", product, [](const std::string& a, const std::string& b) { return "dot(" + a + ", " + b + ")"; },
                                      [](const std::string& a, const std::string& ad, const std::string& b, const std::string& bd)
                                      { return "(dot(" + ad + ", " + b + ") + dot(" + a + ", " + bd + "))"; }))
                             return false;
                         product.Components = 1;
                         args.Out[0] = product;
                         return true;
                     });
            Register("Cross", "Vector", { V("A"), V("B") }, { V("Result") }, {},
                     [](EmitArgs& args)
                     {
                         const Value a = Convert(args.In[0], 3), b = Convert(args.In[1], 3);
                         args.Out[0] = Constant("cross(" + a.Code + ", " + b.Code + ")", 3);
                         return true;
                     });
            Register("Length", "Vector", { V("Value") }, { V("Result") }, {},
                     [](EmitArgs& args)
                     {
                         args.Out[0] = Unary(args.In[0], [](const std::string& v) { return "length(" + v + ")"; },
                                             [](const std::string& v, const std::string& d) { return "(dot(" + v + ", " + d + ") / max(length(" + v + "), 1e-6))"; }, 1);
                         return true;
                     });
            Register("Distance", "Vector", { V("A"), V("B") }, { V("Result") }, {},
                     [](EmitArgs& args)
                     {
                         Value difference;
                         if (!Combine(args.Ctx, args.In[0], args.In[1], "Distance", difference,
                                      [](const std::string& a, const std::string& b) { return "(" + a + " - " + b + ")"; },
                                      [](const std::string&, const std::string& ad, const std::string&, const std::string& bd) { return "(" + ad + " - " + bd + ")"; }))
                             return false;
                         const Value stored = args.Ctx.Store(difference);
                         args.Out[0] = Unary(stored, [](const std::string& v) { return "length(" + v + ")"; },
                                             [](const std::string& v, const std::string& d) { return "(dot(" + v + ", " + d + ") / max(length(" + v + "), 1e-6))"; }, 1);
                         return true;
                     });
        }

        void RegisterVectors()
        {
            // Append: B's components after A's (float + float = float2 ...), at most 4.
            Register("Append", "Vector", { V("A"), V("B") }, { V("Result") }, { { "A", 0.0f }, { "B", 0.0f } },
                     [](EmitArgs& args)
                     {
                         const Value& a = args.In[0];
                         const Value& b = args.In[1];
                         const int components = a.Components + b.Components;
                         if (components > 4)
                         {
                             args.Ctx.Error = std::format("Append makes {} components (at most 4)", components);
                             return false;
                         }
                         const std::string type = TypeName(components);
                         Value out{ type + "(" + a.Code + ", " + b.Code + ")", {}, {}, components };
                         if (a.HasDerivatives() || b.HasDerivatives())
                         {
                             out.Dx = type + "(" + (a.HasDerivatives() ? a.Dx : Zero(a.Components)) + ", " + (b.HasDerivatives() ? b.Dx : Zero(b.Components)) + ")";
                             out.Dy = type + "(" + (a.HasDerivatives() ? a.Dy : Zero(a.Components)) + ", " + (b.HasDerivatives() ? b.Dy : Zero(b.Components)) + ")";
                         }
                         args.Out[0] = out;
                         return true;
                     });
            // Component Mask (Unreal's): the components ticked, in order.
            Register("Component Mask", "Vector", { V("Value") }, { V("Result") }, { { "R", true }, { "G", true }, { "B", false }, { "A", false } },
                     [](EmitArgs& args)
                     {
                         std::string mask;
                         const char* names[4] = { "R", "G", "B", "A" };
                         for (int i = 0; i < 4; ++i)
                         {
                             const NodeGraphValue* value = Property(args.Node, names[i]);
                             const bool* on = value ? std::get_if<bool>(value) : nullptr;
                             if (on && *on)
                             {
                                 if (i >= args.In[0].Components && args.In[0].Components != 1)
                                 {
                                     args.Ctx.Error = std::format("Component Mask takes {} of a {}", names[i], TypeName(args.In[0].Components));
                                     return false;
                                 }
                                 mask += "xyzw"[i];
                             }
                         }
                         if (mask.empty())
                         {
                             args.Ctx.Error = "Component Mask has no component ticked";
                             return false;
                         }
                         const Value x = args.In[0].Components == 1 ? Convert(args.In[0], 4) : args.In[0];
                         args.Out[0] = Unary(x, [&](const std::string& v) { return "(" + v + ")." + mask; }, [&](const std::string&, const std::string& d) { return "(" + d + ")." + mask; },
                                             static_cast<int>(mask.size()));
                         return true;
                     });
            // Split: every component on its own pin.
            Register("Split", "Vector", { V("Value") }, { V("X"), V("Y"), V("Z"), V("W") }, {},
                     [](EmitArgs& args)
                     {
                         const Value x = args.Ctx.Store(Convert(args.In[0], 4));
                         for (int i = 0; i < 4; ++i)
                         {
                             const std::string c(1, "xyzw"[i]);
                             args.Out[i] = Unary(x, [&](const std::string& v) { return v + "." + c; }, [&](const std::string&, const std::string& d) { return d + "." + c; }, 1);
                         }
                         return true;
                     });
        }

        void RegisterInputs()
        {
            Register("Texture Coordinate", "Inputs", {}, { V("UV") }, { { "Index", int32_t(0) }, { "Tiling", glm::vec2(1.0f) } },
                     [](EmitArgs& args)
                     {
                         const NodeGraphValue* indexValue = Property(args.Node, "Index");
                         const int32_t* index = indexValue ? std::get_if<int32_t>(indexValue) : nullptr;
                         const std::string uv = index && *index == 1 ? "i.uv1" : "i.uv0";
                         const NodeGraphValue* tilingValue = Property(args.Node, "Tiling");
                         const glm::vec2* tiling = tilingValue ? std::get_if<glm::vec2>(tilingValue) : nullptr;
                         const std::string scale = tiling ? std::format("float2({}, {})", Float(tiling->x), Float(tiling->y)) : "float2(1.0)";
                         args.Out[0] = Value{ "(" + uv + " * " + scale + ")", "(" + uv + "Dx * " + scale + ")", "(" + uv + "Dy * " + scale + ")", 2 };
                         return true;
                     });
            Register("World Position", "Inputs", {}, { V("Position") }, {},
                     [](EmitArgs& args)
                     {
                         args.Out[0] = Value{ "i.position", "i.positionDx", "i.positionDy", 3 };
                         return true;
                     });
            Register("Screen Position", "Inputs", {}, { V("UV") }, {},
                     [](EmitArgs& args)
                     {
                         args.Out[0] = Value{ "i.screenUV", "i.screenUVDx", "i.screenUVDy", 2 };
                         return true;
                     });
            auto simple = [](const char* name, const char* code, int components)
            {
                Register(name, "Inputs", {}, { V(name == std::string("Vertex Color") ? "Color" : "Value") }, {},
                         [code, components](EmitArgs& args)
                         {
                             args.Out[0] = Constant(code, components);
                             return true;
                         });
            };
            simple("Vertex Normal", "i.normal", 3);
            simple("Vertex Color", "i.vertexColor", 4);
            simple("Camera Vector", "i.view", 3);
            simple("Camera Position", "i.cameraPosition", 3);
            simple("Time", "i.time", 1);

            Register("Constant", "Constants", {}, { V("Value") }, { { "Value", 0.0f } },
                     [](EmitArgs& args)
                     {
                         args.Out[0] = Literal(args.Node, "Value");
                         return true;
                     });
            Register("Constant 2 Vector", "Constants", {}, { V("Value") }, { { "Value", glm::vec2(0.0f) } },
                     [](EmitArgs& args)
                     {
                         args.Out[0] = Literal(args.Node, "Value");
                         return true;
                     });
            Register("Color", "Constants", {}, { V("RGB"), V("RGBA") }, { { "Value", glm::vec4(1.0f) } },
                     [](EmitArgs& args)
                     {
                         const Value color = Literal(args.Node, "Value");
                         args.Out[0] = Convert(color, 3);
                         args.Out[1] = Convert(color, 4);
                         return true;
                     });

            Register("Scalar Parameter", "Parameters", {}, { V("Value") }, { { "Name", std::string("Param") }, { "Default", 0.0f } },
                     [](EmitArgs& args)
                     {
                         const std::string name = StringProperty(args.Node, "Name");
                         if (name.empty())
                         {
                             args.Ctx.Error = "a Scalar Parameter without a Name";
                             return false;
                         }
                         const NodeGraphValue* value = Property(args.Node, "Default");
                         const MaterialGraphParameter* parameter = args.Ctx.Parameter(name, MaterialGraphParameter::Kind::Scalar,
                                                                                         glm::vec4(value ? NodeGraphValueToFloat(*value) : 0.0f, 0.0f, 0.0f, 0.0f), 0);
                         if (!parameter)
                             return false;
                         args.Out[0] = Constant("i.material.graphParameters[" + std::to_string(parameter->Slot) + "].x", 1);
                         return true;
                     });
            Register("Vector Parameter", "Parameters", {}, { V("RGB"), V("RGBA") }, { { "Name", std::string("Color") }, { "Default", glm::vec4(1.0f) } },
                     [](EmitArgs& args)
                     {
                         const std::string name = StringProperty(args.Node, "Name");
                         if (name.empty())
                         {
                             args.Ctx.Error = "a Vector Parameter without a Name";
                             return false;
                         }
                         const NodeGraphValue* value = Property(args.Node, "Default");
                         const glm::vec4* v = value ? std::get_if<glm::vec4>(value) : nullptr;
                         const MaterialGraphParameter* parameter = args.Ctx.Parameter(name, MaterialGraphParameter::Kind::Vector, v ? *v : glm::vec4(1.0f), 0);
                         if (!parameter)
                             return false;
                         const std::string slot = "i.material.graphParameters[" + std::to_string(parameter->Slot) + "]";
                         args.Out[0] = Constant(slot + ".rgb", 3);
                         args.Out[1] = Constant(slot, 4);
                         return true;
                     });
        }

        void RegisterTextures()
        {
            // Texture Sample (Unreal's TextureSample / TextureSampleParameter2D): its texture, or a parameter a material overrides when
            // it has a Name; UV unconnected: the mesh's first UV set. The mip from the UV's derivatives (SampleGrad).
            Register("Texture Sample", "Textures", { V("UV") }, { V("RGB"), V("R"), V("G"), V("B"), V("A"), V("RGBA") },
                     { { "Texture", uint64_t(0) }, { "Parameter", std::string() }, { "Normal Map", false } },
                     [](EmitArgs& args)
                     {
                         const NodeGraphValue* textureValue = Property(args.Node, "Texture");
                         const uint64_t* texture = textureValue ? std::get_if<uint64_t>(textureValue) : nullptr;
                         std::string name = StringProperty(args.Node, "Parameter");
                         if (name.empty())
                             name = "#" + std::to_string(args.NodeId); // (its own slot, not overridable)
                         const MaterialGraphParameter* parameter = args.Ctx.Parameter(name, MaterialGraphParameter::Kind::Texture, glm::vec4(0.0f), texture ? *texture : 0);
                         if (!parameter)
                             return false;
                         const NodeGraphValue* normalValue = Property(args.Node, "Normal Map");
                         const bool* normalMap = normalValue ? std::get_if<bool>(normalValue) : nullptr;
                         const Value uv = args.Connected[0] ? Convert(args.In[0], 2) : Value{ "i.uv0", "i.uv0Dx", "i.uv0Dy", 2 };
                         const std::string dx = uv.HasDerivatives() ? uv.Dx : "float2(0.0)", dy = uv.HasDerivatives() ? uv.Dy : "float2(0.0)";
                         const Value sample = args.Ctx.Store(Constant(std::format("material_graph_texture(i, {}, {}, {}, {}, {})", parameter->Slot, uv.Code, dx, dy,
                                                                                  normalMap && *normalMap ? "true" : "false"), 4));
                         args.Out[0] = Constant(sample.Code + ".rgb", 3);
                         for (int c = 0; c < 4; ++c)
                             args.Out[1 + c] = Constant(sample.Code + "." + std::string(1, "xyzw"[c]), 1);
                         args.Out[5] = sample;
                         return true;
                     },
                     { { "Texture", "AssetType::Texture2D" } });
        }

        void RegisterUtility()
        {
            // Panner: UV + Speed x Time (scrolling water, conveyor belts).
            Register("Panner", "Utility", { V("UV"), V("Time"), V("Speed") }, { V("Result") }, { { "Speed", glm::vec2(0.1f, 0.0f) } },
                     [](EmitArgs& args)
                     {
                         const Value uv = args.Connected[0] ? Convert(args.In[0], 2) : Value{ "i.uv0", "i.uv0Dx", "i.uv0Dy", 2 };
                         const Value time = args.Connected[1] ? args.In[1] : Constant("i.time", 1);
                         const Value speed = Convert(args.In[2], 2);
                         args.Out[0] = Value{ "(" + uv.Code + " + " + speed.Code + " * " + time.Code + ")", uv.Dx, uv.Dy, 2 };
                         return true;
                     });
            // Fresnel: Base Reflect Fraction + (1 - it) x (1 - N.V)^Exponent, N the vertex normal unless connected.
            Register("Fresnel", "Utility", { V("Exponent"), V("Base Reflect Fraction"), V("Normal") }, { V("Result") },
                     { { "Exponent", 5.0f }, { "Base Reflect Fraction", 0.04f } },
                     [](EmitArgs& args)
                     {
                         const Value exponent = Convert(args.In[0], 1), base = Convert(args.In[1], 1);
                         const std::string normal = args.Connected[2] ? "normalize(" + Convert(args.In[2], 3).Code + ")" : "i.normal";
                         args.Out[0] = Constant("(" + base.Code + " + (1.0 - " + base.Code + ") * pow(1.0 - saturate(dot(" + normal + ", i.view)), " + exponent.Code + "))", 1);
                         return true;
                     });
            Register("Desaturation", "Utility", { V("Color"), V("Fraction") }, { V("Result") }, { { "Fraction", 1.0f } },
                     [](EmitArgs& args)
                     {
                         const Value color = Convert(args.In[0], 3), fraction = Convert(args.In[1], 1);
                         args.Out[0] = Constant("lerp(" + color.Code + ", float3(dot(" + color.Code + ", float3(0.2126, 0.7152, 0.0722))), " + fraction.Code + ")", 3);
                         return true;
                     });
            // Noise: smooth value noise in [0, 1] at Position x Scale (a float2 or float3 position).
            Register("Noise", "Utility", { V("Position"), V("Scale") }, { V("Result") }, { { "Scale", 1.0f } },
                     [](EmitArgs& args)
                     {
                         const Value position = args.Connected[0] ? Convert(args.In[0], 3) : Value{ "float3(i.uv0, 0.0)", {}, {}, 3 };
                         const Value scale = Convert(args.In[1], 1);
                         args.Out[0] = Constant("material_graph_noise(" + position.Code + " * " + scale.Code + ")", 1);
                         return true;
                     });
        }

        void RegisterOutput()
        {
            Register("Material Output", "Output",
                     { V("Base Color"), V("Metallic"), V("Roughness"), V("Normal"), V("Emissive"), V("Ambient Occlusion"), V("Opacity") }, {}, {},
                     [](EmitArgs& args)
                     {
                         struct Pin { const char* Field; int Components; uint32_t Bit; const char* Before; const char* After; };
                         const Pin pins[] = {
                             { "baseColor", 3, MaterialGraphOutputBaseColor, "max(", ", float3(0.0))" },
                             { "metallic", 1, MaterialGraphOutputMetallic, "saturate(", ")" },
                             { "roughness", 1, MaterialGraphOutputRoughness, "saturate(", ")" },
                             { "normal", 3, MaterialGraphOutputNormal, "(", ")" }, // (tangent space: the caller normalizes and applies the frame)
                             { "emissive", 3, MaterialGraphOutputEmissive, "max(", ", float3(0.0))" },
                             { "occlusion", 1, MaterialGraphOutputOcclusion, "saturate(", ")" },
                             { "opacity", 1, MaterialGraphOutputOpacity, "saturate(", ")" },
                         };
                         for (size_t pin = 0; pin < std::size(pins); ++pin)
                         {
                             if (!args.Connected[pin])
                                 continue; // (keeps the material's own value)
                             const Value value = Convert(args.In[pin], pins[pin].Components);
                             args.Ctx.Body += std::string("    o.") + pins[pin].Field + " = " + pins[pin].Before + value.Code + pins[pin].After + ";\n";
                             args.Ctx.Outputs |= pins[pin].Bit;
                         }
                         return true;
                     });
        }
    }

    void RegisterMaterialGraphNodeTypes()
    {
        static bool registered = false;
        if (registered)
            return;
        registered = true;
        RegisterArithmetic();
        RegisterVectors();
        RegisterInputs();
        RegisterTextures();
        RegisterUtility();
        RegisterOutput();
        RegisterRerouteNodeType(kMaterialGraphDomain);
    }

    NodeGraph CreateEmptyMaterialGraph()
    {
        NodeGraph graph;
        graph.Domain = kMaterialGraphDomain;
        GraphNode output;
        output.Id = graph.NextNodeId++;
        output.TypeName = "Material Output";
        output.EditorPosition = { 400.0f, 0.0f };
        graph.Nodes.push_back(output);
        graph.OutputNode = output.Id;
        return graph;
    }

    MaterialGraphProgram CompileMaterialGraph(const NodeGraph& graph)
    {
        MaterialGraphProgram program;
        if (graph.Domain != kMaterialGraphDomain)
        {
            program.Error = std::format("not a material graph (its domain is '{}')", graph.Domain);
            return program;
        }
        const CompiledGraph compiled = GraphCompiler::Compile(graph);
        if (!compiled.IsValid())
        {
            program.Error = "the graph does not compile: a node of an unknown type, a cycle, or no Material Output (the log has which)";
            return program;
        }
        const CompiledNode& outputNode = compiled.Nodes[compiled.OutputNodeIndex];
        if (!outputNode.Type || outputNode.Type->TypeName != "Material Output")
        {
            program.Error = "the graph's output is not a Material Output node";
            return program;
        }

        // What the output needs (an unconnected node computes nothing, and its errors do not count).
        std::vector<bool> needed(compiled.Nodes.size(), false);
        std::vector<uint32_t> stack{ compiled.OutputNodeIndex };
        while (!stack.empty())
        {
            const uint32_t index = stack.back();
            stack.pop_back();
            if (needed[index])
                continue;
            needed[index] = true;
            for (const CompiledPin& pin : compiled.Nodes[index].Inputs)
                if (pin.Node != kInvalidGraphIndex)
                    stack.push_back(pin.Node);
        }

        Context context;
        std::vector<std::vector<Value>> outputs(compiled.Nodes.size());
        for (uint32_t index = 0; index < compiled.Nodes.size(); ++index)
        {
            if (!needed[index])
                continue;
            const CompiledNode& node = compiled.Nodes[index];
            const auto emitter = Emitters().find(node.Type->TypeName);
            if (emitter == Emitters().end())
            {
                program.Error = std::format("node {} ({}) is not a material node", node.SourceNodeId, node.Type->TypeName);
                return program;
            }
            const std::vector<NodePinDesc> inputPins = ResolveNodePins(*node.Type, node.Properties).Inputs;
            std::vector<Value> in(inputPins.size());
            std::vector<bool> connected(inputPins.size(), false);
            for (size_t pin = 0; pin < inputPins.size(); ++pin)
            {
                const CompiledPin source = pin < node.Inputs.size() ? node.Inputs[pin] : CompiledPin{};
                if (source.Node != kInvalidGraphIndex)
                {
                    const uint32_t slot = source.OutputSlot; // (the source node's output pin)
                    in[pin] = outputs[source.Node].at(slot);
                    connected[pin] = true;
                }
                else
                    in[pin] = Literal(node, inputPins[pin].Name);
            }
            outputs[index].resize(node.Type->Outputs.size());
            EmitArgs args{ context, node, node.SourceNodeId, in, connected, outputs[index] };
            if (!emitter->second(args) || !context.Error.empty())
            {
                program.Error = std::format("node {} ({}): {}", node.SourceNodeId, node.Type->TypeName,
                                            context.Error.empty() ? std::string("could not be generated") : context.Error);
                return program;
            }
            // Every output a node gives into a local: a value read by several nodes is computed once.
            for (Value& value : outputs[index])
                if (!value.Code.empty() && value.Code.find_first_of("(+-*/") != std::string::npos)
                    value = context.Store(value);
        }

        program.Valid = true;
        program.Parameters = std::move(context.Parameters);
        program.ParameterSlots = context.ParameterSlots;
        program.TextureSlots = context.TextureSlots;
        program.Outputs = context.Outputs;
        program.Code = std::format("static const uint Outputs = {}u;\n\nvoid surface(MaterialGraphInputs i, inout MaterialGraphOutputs o)\n{{\n{}}}\n",
                                   context.Outputs, context.Body);
        return program;
    }
}
