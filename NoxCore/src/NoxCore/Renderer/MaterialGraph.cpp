#include "NoxCore/Renderer/MaterialGraph.h"

#include <algorithm>
#include <cctype>
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
            uint32_t Outputs2 = 0;
            std::string Error;
            // Which function is being generated: surface() (every Material Output pin but Opacity Mask and World Position Offset),
            // opacity_mask() or world_position_offset() (that pin alone; the Material Output then writes `return`). The offset is
            // computed per vertex: no screen derivatives, nothing that needs the pixel on screen.
            // custom_light() / custom_ambient(): the Custom Light / Custom Ambient pins (the Lit Surface, Light and Ambient Light nodes
            // only there).
            enum class Function { Surface, OpacityMask, PixelDepthOffset, WorldPositionOffset, VertexOutputs, CustomLight, CustomAmbient };
            Function Pass = Function::Surface;
            // Vertex Interpolator slot assignment (step 4f): node Id -> slot 0..3 and component count 1..4.
            std::unordered_map<uint32_t, uint32_t> InterpolatorSlots;
            std::unordered_map<uint32_t, int> InterpolatorComponents;
            // Expression read by Pixel Normal (step 4h): "_pixelNormal" when the graph also connects Material Output::Normal, else "i.pixelNormal".
            std::string PixelNormalExpr = "i.pixelNormal";

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
        // A pin with the editor's tooltip (its shader stage: what runs per vertex, per pixel or per light -- plan step 4d).
        NodePinDesc V(const std::string& name, const std::string& tooltip) { return { name, "Value", tooltip }; }

        void Register(const std::string& name, const std::string& category, std::vector<NodePinDesc> inputs, std::vector<NodePinDesc> outputs,
                      std::vector<std::pair<std::string, NodeGraphValue>> properties, EmitFn emit,
                      std::unordered_map<std::string, std::string> assetHints = {},
                      std::unordered_map<std::string, std::vector<std::string>> propertyEnums = {})
        {
            NodeTypeDesc desc;
            desc.TypeName = name;
            desc.Domain = kMaterialGraphDomain;
            desc.Category = category;
            desc.Inputs = std::move(inputs);
            desc.Outputs = std::move(outputs);
            desc.DefaultProperties = std::move(properties);
            desc.PropertyAssetTypeHints = std::move(assetHints);
            desc.PropertyEnums = std::move(propertyEnums);
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
            Register("Texture Coordinate", "Inputs", {}, { V("UV", "The mesh's UV set (per vertex; interpolated per pixel; in Custom Light the first set)") }, { { "Index", int32_t(0) }, { "Tiling", glm::vec2(1.0f) } },
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
            Register("World Position", "Inputs", {}, { V("Position", "World position, after skinning and World Position Offset (per vertex; per pixel the pixel's)") }, {},
                     [](EmitArgs& args)
                     {
                         args.Out[0] = Value{ "i.position", "i.positionDx", "i.positionDy", 3 };
                         return true;
                     });
            // The mesh's own data per vertex (stage: vertex; interpolated across the triangle in the pixel stage). The Custom Lit pass lights
            // from the G-buffer, which keeps none of them: refused in Custom Light / Custom Ambient.
            auto meshDataOnly = [](EmitArgs& args, const char* node)
            {
                if (args.Ctx.Pass != Context::Function::CustomLight && args.Ctx.Pass != Context::Function::CustomAmbient)
                    return true;
                args.Ctx.Error = std::format("{} cannot feed Custom Light or Custom Ambient (the lighting runs from the G-buffer, which does not keep it)", node);
                return false;
            };
            Register("Local Position", "Inputs", {}, { V("Position", "The vertex in the mesh's own space before skinning and morph targets (per vertex; interpolated per pixel). Not in Custom Light / Custom Ambient") }, {},
                     [meshDataOnly](EmitArgs& args)
                     {
                         if (!meshDataOnly(args, "Local Position"))
                             return false;
                         // (Unreal's Pre-Skinned Local Position: the mesh's vertex before skinning and morph targets)
                         args.Out[0] = Value{ "i.localPosition", "i.localPositionDx", "i.localPositionDy", 3 };
                         return true;
                     });
            Register("Vertex Tangent", "Inputs", {}, { V("Tangent", "The mesh's tangent in world space, skinned (per vertex; interpolated per pixel). Not in Custom Light / Custom Ambient") }, {},
                     [meshDataOnly](EmitArgs& args)
                     {
                         if (!meshDataOnly(args, "Vertex Tangent"))
                             return false;
                         args.Out[0] = Constant("(dot(i.tangent.xyz, i.tangent.xyz) > 1e-12 ? normalize(i.tangent.xyz) : float3(0.0))", 3);
                         return true;
                     });
            Register("Vertex Bitangent", "Inputs", {}, { V("Bitangent", "Across the surface, perpendicular to the normal and the tangent (per vertex; interpolated per pixel). Not in Custom Light / Custom Ambient") }, {},
                     [meshDataOnly](EmitArgs& args)
                     {
                         if (!meshDataOnly(args, "Vertex Bitangent"))
                             return false;
                         // (the binormal: across the surface, perpendicular to the normal and the tangent, its sign the tangent's handedness)
                         args.Out[0] = Constant("(dot(i.tangent.xyz, i.tangent.xyz) > 1e-12 ? normalize(cross(i.normal, i.tangent.xyz)) * (i.tangent.w < 0.0 ? -1.0 : 1.0) : float3(0.0))", 3);
                         return true;
                     });
            // Step 4e part 2: Object Position, Object Bounds, Space Transform, and Model / View / Projection matrices.
            Register("Object Position", "Inputs", {},
                     { V("Position", "The object's pivot in world space (Float3). Per vertex / per pixel; not in Custom Light / Custom Ambient") }, {},
                     [meshDataOnly](EmitArgs& args)
                     {
                         if (!meshDataOnly(args, "Object Position"))
                             return false;
                         args.Out[0] = Constant("material_graph_object_position(i)", 3);
                         return true;
                     });
            Register("Object Bounds", "Inputs", {},
                     { V("Center", "World-space centre of the object's bounding box (Float3). Not in Custom Light / Custom Ambient"),
                       V("Extent", "World-space half-size of the object's bounding box (Float3). Not in Custom Light / Custom Ambient"),
                       V("Radius", "World-space bounding sphere radius (Float). Not in Custom Light / Custom Ambient") }, {},
                     [meshDataOnly](EmitArgs& args)
                     {
                         if (!meshDataOnly(args, "Object Bounds"))
                             return false;
                         args.Out[0] = Constant("material_graph_object_bounds_center(i)", 3);
                         const Value extent = args.Ctx.Store(Constant("material_graph_object_bounds_extent(i)", 3));
                         args.Out[1] = extent;
                         args.Out[2] = Constant("length(" + extent.Code + ")", 1);
                         return true;
                     });
            Register("Transform", "Vector",
                     { V("Value", "Vector or position to transform (Float3)") },
                     { V("Result", "Transformed vector or position in the target space (Float3)") },
                     { { "From", int32_t(0) }, { "To", int32_t(1) }, { "Kind", int32_t(0) }, { "Value", glm::vec4(0.0f) } },
                     [meshDataOnly](EmitArgs& args)
                     {
                         auto intProp = [&](const char* key, int32_t fallback) -> int32_t
                         {
                             if (const NodeGraphValue* v = Property(args.Node, key))
                                 if (const int32_t* n = std::get_if<int32_t>(v))
                                     return *n;
                             return fallback;
                         };
                         const int32_t fromSpace = std::clamp(intProp("From", 0), 0, 3);
                         const int32_t toSpace = std::clamp(intProp("To", 1), 0, 3);
                         const int32_t kind = std::clamp(intProp("Kind", 0), 0, 1);
                         if ((fromSpace == 0 || toSpace == 0) && !meshDataOnly(args, "Transform (Local space)"))
                             return false;
                         const Value v = Convert(args.In[0], 3);
                         args.Out[0] = Constant(std::format("material_graph_transform(i, {}, {}u, {}u, {})",
                                                            v.Code, static_cast<uint32_t>(fromSpace), static_cast<uint32_t>(toSpace),
                                                            kind == 1 ? "true" : "false"), 3);
                         return true;
                     },
                     {},
                     { { "From", { "Local", "World", "View", "Clip" } },
                       { "To", { "Local", "World", "View", "Clip" } },
                       { "Kind", { "Direction", "Position" } } });
            Register("Matrix", "Inputs", {},
                     { V("Row 0", "First row of the 4x4 matrix (Float4)"),
                       V("Row 1", "Second row of the 4x4 matrix (Float4)"),
                       V("Row 2", "Third row of the 4x4 matrix (Float4)"),
                       V("Row 3", "Fourth row of the 4x4 matrix (Float4)") },
                     { { "Matrix", int32_t(0) } },
                     [meshDataOnly](EmitArgs& args)
                     {
                         int32_t kind = 0;
                         if (const NodeGraphValue* v = Property(args.Node, "Matrix"))
                             if (const int32_t* n = std::get_if<int32_t>(v))
                                 kind = std::clamp(*n, 0, 7);
                         if ((kind == 0 || kind == 1) && !meshDataOnly(args, "Matrix (Model)"))
                             return false;
                         const std::string expr = std::format("material_graph_matrix(i, {}u)", static_cast<uint32_t>(kind));
                         args.Out[0] = Constant("(" + expr + ")[0]", 4);
                         args.Out[1] = Constant("(" + expr + ")[1]", 4);
                         args.Out[2] = Constant("(" + expr + ")[2]", 4);
                         args.Out[3] = Constant("(" + expr + ")[3]", 4);
                         return true;
                     },
                     {},
                     { { "Matrix", { "Model", "Inverse Model", "View", "Inverse View", "Projection", "Inverse Projection", "View Projection", "Inverse View Projection" } } });
            Register("Screen Position", "Inputs", {}, { V("UV", "Where the pixel is on the screen, 0..1 (per pixel only: not in World Position Offset or Vertex Outputs)") }, {},
                     [](EmitArgs& args)
                     {
                         if (args.Ctx.Pass == Context::Function::WorldPositionOffset || args.Ctx.Pass == Context::Function::VertexOutputs)
                         {
                             // (Unreal's rule too: vertex-stage outputs run per vertex, before the vertex has a place on the screen)
                             args.Ctx.Error = "Screen Position cannot feed World Position Offset or vertex-stage outputs (computed per vertex, before it is on screen)";
                             return false;
                         }
                         args.Out[0] = Value{ "i.screenUV", "i.screenUVDx", "i.screenUVDy", 2 };
                         return true;
                     });
            // Step 4h: Pixel-stage inputs -- Pixel Normal (after normal map / Normal pin), Scene Depth, Pixel Depth.
            auto pixelStageOnly = [](EmitArgs& args, const char* node)
            {
                if (args.Ctx.Pass == Context::Function::WorldPositionOffset || args.Ctx.Pass == Context::Function::VertexOutputs)
                {
                    args.Ctx.Error = std::format("{} cannot feed World Position Offset or vertex-stage outputs (computed per pixel)", node);
                    return false;
                }
                return true;
            };
            Register("Pixel Normal", "Inputs", {},
                     { V("Normal", "The shading normal in world space after normal mapping or the Normal pin (Float3; per pixel only)") }, {},
                     [pixelStageOnly](EmitArgs& args)
                     {
                         if (!pixelStageOnly(args, "Pixel Normal"))
                             return false;
                         args.Out[0] = Constant(args.Ctx.PixelNormalExpr, 3);
                         return true;
                     });
            Register("Scene Depth", "Inputs", {},
                     { V("Depth", "Linear distance from the camera to the opaque scene behind this pixel, in world units (Float; per pixel only)") }, {},
                     [pixelStageOnly](EmitArgs& args)
                     {
                         if (!pixelStageOnly(args, "Scene Depth"))
                             return false;
                         args.Out[0] = Constant("i.sceneDepth", 1);
                         return true;
                     });
            Register("Pixel Depth", "Inputs", {},
                     { V("Depth", "Linear distance from the camera to this pixel, in world units (Float; per pixel only)") }, {},
                     [pixelStageOnly](EmitArgs& args)
                     {
                         if (!pixelStageOnly(args, "Pixel Depth"))
                             return false;
                         args.Out[0] = Constant("i.pixelDepth", 1);
                         return true;
                     });
            auto simple = [](const char* name, const char* code, int components, const char* tooltip)
            {
                Register(name, "Inputs", {}, { V(name == std::string("Vertex Color") ? "Color" : "Value", tooltip) }, {},
                         [code, components](EmitArgs& args)
                         {
                             args.Out[0] = Constant(code, components);
                             return true;
                         });
            };
            simple("Vertex Normal", "i.normal", 3, "The mesh's normal in world space, skinned (per vertex; interpolated per pixel; in Custom Light the shading normal)");
            simple("Vertex Color", "i.vertexColor", 4, "The mesh's vertex colour, white without (per vertex; interpolated per pixel; white in Custom Light)");
            simple("Camera Vector", "i.view", 3, "From the surface towards the camera (every stage)");
            simple("Camera Position", "i.cameraPosition", 3, "The camera's world position (every stage)");
            simple("Time", "i.time", 1, "Seconds (every stage; World Position Offset's motion vectors use last frame's too)");

            // Custom Lit lighting (Custom Light / Custom Ambient): the surface the lighting sees (the G-buffer's, after the graph's own
            // surface pins), the light being added, the light from all around -- CustomLit.slang's CustomLitSurface, CustomLitLight and
            // CustomLitAmbient.
            auto lightingOnly = [](EmitArgs& args, const char* node, bool light, bool ambient)
            {
                const bool allowed = (light && args.Ctx.Pass == Context::Function::CustomLight) || (ambient && args.Ctx.Pass == Context::Function::CustomAmbient);
                if (!allowed)
                    args.Ctx.Error = std::format("{} can only feed {}", node, light && ambient ? "Custom Light or Custom Ambient" : light ? "Custom Light" : "Custom Ambient");
                return allowed;
            };
            Register("Lit Surface", "Lighting", {}, { V("Base Color", "Per light: the surface as the lighting sees it (after the graph's own pins)"), V("Normal", "Per light: the shading normal"),
                                                     V("View", "Per light: towards the camera"), V("Metallic", "Per light"), V("Roughness", "Per light"), V("Occlusion", "Per light: the material's AO times SSAO / GTAO / RTAO") }, {},
                     [lightingOnly](EmitArgs& args)
                     {
                         if (!lightingOnly(args, "Lit Surface", true, true))
                             return false;
                         args.Out[0] = Constant("s.baseColor", 3);
                         args.Out[1] = Constant("s.normal", 3);
                         args.Out[2] = Constant("s.view", 3);
                         args.Out[3] = Constant("s.metallic", 1);
                         args.Out[4] = Constant("s.roughness", 1);
                         args.Out[5] = Constant("s.occlusion", 1);
                         return true;
                     });
            Register("Light", "Lighting", {}, { V("Direction", "Per light: towards the light"), V("Color", "Per light: its colour x intensity x distance and cone falloff"),
                                                V("Shadow", "Per light: 1 lit .. 0 in shadow"), V("Distance", "Per light: world units to the light") }, {},
                     [lightingOnly](EmitArgs& args)
                     {
                         if (!lightingOnly(args, "Light", true, false))
                             return false;
                         args.Out[0] = Constant("l.direction", 3);
                         args.Out[1] = Constant("l.color", 3);
                         args.Out[2] = Constant("l.shadow", 1);
                         args.Out[3] = Constant("l.distance", 1);
                         return true;
                     });
            Register("Ambient Light", "Lighting", {}, { V("Diffuse", "Custom Ambient: what a white matte surface here gets from all around"),
                                                        V("Reflection", "Custom Ambient: the surroundings in the mirror direction, blurred by the roughness") }, {},
                     [lightingOnly](EmitArgs& args)
                     {
                         if (!lightingOnly(args, "Ambient Light", false, true))
                             return false;
                         args.Out[0] = Constant("a.diffuse", 3);
                         args.Out[1] = Constant("a.reflection", 3);
                         return true;
                     });

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
                     // "Parameter Name" (Unreal's label), not "Parameter": that property name marks a node reading a graph variable
                     // (kParameterReferenceProperty), and the node graph editor's Add menu offered the node only as "Get <variable>"
                     // -- material graphs have none, so Texture Sample was missing from the menu (the user's test).
                     { { "Texture", uint64_t(0) }, { "Parameter Name", std::string() }, { "Normal Map", false } },
                     [](EmitArgs& args)
                     {
                         const NodeGraphValue* textureValue = Property(args.Node, "Texture");
                         const uint64_t* texture = textureValue ? std::get_if<uint64_t>(textureValue) : nullptr;
                         std::string name = StringProperty(args.Node, "Parameter Name");
                         if (name.empty())
                             name = StringProperty(args.Node, kParameterReferenceProperty); // (a graph saved before the rename)
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
            // Depth Fade (step 4h): fades translucent opacity smoothly to 0 where the surface meets opaque geometry (Scene Depth - Pixel Depth).
            Register("Depth Fade", "Utility",
                     { V("Opacity", "Base opacity 0..1 (Float)"),
                       V("Fade Distance", "Distance in world units over which the surface fades in where it meets opaque geometry (Float)") },
                     { V("Result", "Opacity * saturate((Scene Depth - Pixel Depth) / Fade Distance) (Float; per pixel only)") },
                     { { "Opacity", 1.0f }, { "Fade Distance", 1.0f } },
                     [](EmitArgs& args)
                     {
                         if (args.Ctx.Pass == Context::Function::WorldPositionOffset || args.Ctx.Pass == Context::Function::VertexOutputs)
                         {
                             args.Ctx.Error = "Depth Fade cannot feed World Position Offset or vertex-stage outputs (computed per pixel)";
                             return false;
                         }
                         const Value opacity = Convert(args.In[0], 1);
                         const Value fadeDistance = Convert(args.In[1], 1);
                         args.Out[0] = Constant("(" + opacity.Code + " * saturate((i.sceneDepth - i.pixelDepth) / max(" + fadeDistance.Code + ", 1e-5)))", 1);
                         return true;
                     });
            // Vertex Interpolator (step 4f, Unreal's Vertex Interpolator / Customized UVs): evaluates its input once per vertex and
            // interpolates the result across the triangle into the pixel stage (up to 4 float4 slots per graph).
            Register("Vertex Interpolator", "Utility",
                     { V("Value", "Per vertex: evaluated once per vertex and interpolated across the triangle (Float..Float4)") },
                     { V("Result", "Per pixel: interpolated value from the vertex stage, with screen derivatives (Float..Float4)") },
                     { { "Value", 0.0f } },
                     [](EmitArgs& args)
                     {
                         if (args.Ctx.Pass == Context::Function::CustomLight || args.Ctx.Pass == Context::Function::CustomAmbient)
                         {
                             args.Ctx.Error = "Vertex Interpolator cannot feed Custom Light / Custom Ambient (the lighting pass runs from the G-buffer)";
                             return false;
                         }
                         if (args.Ctx.Pass == Context::Function::WorldPositionOffset)
                         {
                             args.Out[0] = args.In[0];
                             return true;
                         }
                         const auto slotIt = args.Ctx.InterpolatorSlots.find(args.NodeId);
                         if (slotIt == args.Ctx.InterpolatorSlots.end())
                         {
                             args.Out[0] = args.In[0];
                             return true;
                         }
                         const uint32_t slot = slotIt->second;
                         if (args.Ctx.Pass == Context::Function::VertexOutputs)
                         {
                             args.Ctx.InterpolatorComponents[args.NodeId] = args.In[0].Components;
                             const Value v4 = Convert(args.In[0], 4);
                             args.Ctx.Body += std::format("    vo.interp[{}u] = {};\n", slot, v4.Code);
                             args.Ctx.Outputs |= MaterialGraphOutputVertexInterpolators;
                             args.Out[0] = args.In[0];
                             return true;
                         }
                         // Surface / OpacityMask: read the interpolated slot with its screen derivatives.
                         const int comps = args.Ctx.InterpolatorComponents.contains(args.NodeId)
                             ? args.Ctx.InterpolatorComponents.at(args.NodeId) : 4;
                         const std::string swizzle = comps == 1 ? ".x" : comps == 2 ? ".xy" : comps == 3 ? ".xyz" : "";
                         const std::string base = std::format("i.vertexInterp[{}u]{}", slot, swizzle);
                         const std::string dx = std::format("i.vertexInterpDx[{}u]{}", slot, swizzle);
                         const std::string dy = std::format("i.vertexInterpDy[{}u]{}", slot, swizzle);
                         args.Out[0] = Value{ base, dx, dy, comps };
                         return true;
                     });
        }

        void RegisterOutput()
        {
            Register("Material Output", "Output",
                     { V("Base Color", "Per pixel: the surface's colour (metals: their reflection colour) (Float3)"),
                       V("Metallic", "Per pixel: 0 non-metal .. 1 metal (Float)"),
                       V("Roughness", "Per pixel: 0 mirror .. 1 matte (Float)"),
                       V("Normal", "Per pixel: the surface direction in tangent space (+Z out of the surface), e.g. from a normal map (Float3)"),
                       V("Emissive", "Per pixel: light the surface gives off itself (Float3)"),
                       V("Ambient Occlusion", "Per pixel: 1 open .. 0 enclosed (darkens the ambient light) (Float)"),
                       V("Opacity", "Per pixel: Translucent materials only, 1 solid .. 0 clear (Float)"),
                       V("Opacity Mask", "Per pixel: Masked materials only -- below the Opacity Mask Clip Value the pixel is cut away (shadows and ray tracing too) (Float)"),
                       V("World Position Offset", "Per vertex: moves each vertex (world units), before the triangle is drawn -- every pass and the ray tracing scene (Float3)"),
                       V("Custom Light", "Per light (Custom Lit materials only): what one light adds to the pixel, summed over the lights (Float3)"),
                       V("Custom Ambient", "Per light pass, once (Custom Lit materials only): the light from all around -- sky, GI, reflections (Float3)"),
                       V("Vertex Normal", "Per vertex: replaces the mesh's world-space vertex normal before interpolation across the triangle (Float3)"),
                       V("Vertex Color", "Per vertex: replaces the mesh's vertex colour before interpolation across the triangle (Float4)"),
                       V("Specular", "Per pixel: dielectric specular weight 0..1 (Float)"),
                       V("Specular Color", "Per pixel: dielectric specular tint colour (Float3)"),
                       V("Clear Coat", "Per pixel: clear coat layer strength 0..1 (Float)"),
                       V("Clear Coat Roughness", "Per pixel: clear coat roughness 0 mirror .. 1 matte (Float)"),
                       V("Clear Coat Normal", "Per pixel: clear coat tangent-space normal (+Z out of the surface) (Float3)"),
                       V("Sheen Color", "Per pixel: fabric/velvet grazing sheen colour (Float3)"),
                       V("Sheen Roughness", "Per pixel: sheen roughness 0..1 (Float)"),
                       V("Anisotropy", "Per pixel: brushed-metal highlight stretch 0..1 (Float)"),
                       V("Anisotropy Direction", "Per pixel: tangent-space anisotropy direction (Float2)"),
                       V("Transmission", "Per pixel: glass/liquid transmission 0..1 (Float)"),
                       V("IOR", "Per pixel: index of refraction, >= 1.0 (default 1.5) (Float)"),
                       V("Thickness", "Per pixel: volume thickness for refraction & absorption (Float)"),
                       V("Attenuation Color", "Per pixel: volume absorption colour (Float3)"),
                       V("Attenuation Distance", "Per pixel: distance at which light reaches Attenuation Color (Float)"),
                       V("Diffuse Transmission", "Per pixel: thin-surface back-lit transmission 0..1 (Float)"),
                       V("Iridescence", "Per pixel: thin-film iridescence strength 0..1 (Float)"),
                       V("Iridescence Thickness", "Per pixel: thin-film thickness in nanometers, e.g. 100..800 (Float)"),
                       V("Dispersion", "Per pixel: chromatic dispersion strength >= 0 (Float)"),
                       V("Subsurface Color", "Per pixel (Subsurface shading model): colour of light scattered beneath the surface (skin, wax, marble) (Float3)"),
                       V("AO Light Affect", "Per pixel: how much Ambient Occlusion darkens direct light, 0 ambient only .. 1 full direct darkening (Float)"),
                       V("Pixel Depth Offset", "Per pixel: pushes the pixel further away along the view ray in world units >= 0 (Float)") }, {},
                     // (appended after the pins: links name pins by index, so graphs saved before World Position Offset keep theirs)
                     { { "Max World Position Offset Displacement", 0.0f } },
                     [](EmitArgs& args)
                     {
                         constexpr size_t maskPin = 7;
                         constexpr size_t offsetPin = 8;
                         constexpr size_t lightPin = 9;
                         constexpr size_t ambientPin = 10;
                         constexpr size_t vertexNormalPin = 11;
                         constexpr size_t vertexColorPin = 12;
                         if (args.Ctx.Pass == Context::Function::VertexOutputs)
                         {
                             if (args.Connected.size() > vertexNormalPin && args.Connected[vertexNormalPin])
                             {
                                 const Value n = args.Ctx.Store(Convert(args.In[vertexNormalPin], 3));
                                 args.Ctx.Body += "    vo.normal = dot(" + n.Code + ", " + n.Code + ") > 1e-12 ? normalize(" + n.Code + ") : vo.normal;\n";
                                 args.Ctx.Outputs |= MaterialGraphOutputVertexNormal;
                             }
                             if (args.Connected.size() > vertexColorPin && args.Connected[vertexColorPin])
                             {
                                 args.Ctx.Body += "    vo.color = " + Convert(args.In[vertexColorPin], 4).Code + ";\n";
                                 args.Ctx.Outputs |= MaterialGraphOutputVertexColor;
                             }
                             return true;
                         }
                         if (args.Ctx.Pass == Context::Function::CustomLight)
                         {
                             if (args.Connected.size() > lightPin && args.Connected[lightPin])
                             {
                                 args.Ctx.Body += "    return max(" + Convert(args.In[lightPin], 3).Code + ", float3(0.0));\n";
                                 args.Ctx.Outputs |= MaterialGraphOutputCustomLight;
                             }
                             else
                                 args.Ctx.Body += "    return custom_lit_lambert(s, l);\n"; // (Default Lit's diffuse, as a Material Shader starts)
                             return true;
                         }
                         if (args.Ctx.Pass == Context::Function::CustomAmbient)
                         {
                             if (args.Connected.size() > ambientPin && args.Connected[ambientPin])
                             {
                                 args.Ctx.Body += "    return max(" + Convert(args.In[ambientPin], 3).Code + ", float3(0.0));\n";
                                 args.Ctx.Outputs |= MaterialGraphOutputCustomAmbient;
                             }
                             else
                                 args.Ctx.Body += "    return s.baseColor * a.diffuse * s.occlusion;\n";
                             return true;
                         }
                         if (args.Ctx.Pass == Context::Function::WorldPositionOffset)
                         {
                             if (args.Connected.size() > offsetPin && args.Connected[offsetPin])
                             {
                                 args.Ctx.Body += "    return " + Convert(args.In[offsetPin], 3).Code + ";\n";
                                 args.Ctx.Outputs |= MaterialGraphOutputWorldPositionOffset;
                             }
                             else
                                 args.Ctx.Body += "    return float3(0.0);\n";
                             return true;
                         }
                         if (args.Ctx.Pass == Context::Function::OpacityMask)
                         {
                             if (args.Connected.size() > maskPin && args.Connected[maskPin])
                             {
                                 args.Ctx.Body += "    return " + Convert(args.In[maskPin], 1).Code + ";\n";
                                 args.Ctx.Outputs |= MaterialGraphOutputOpacityMask;
                             }
                             else
                                 args.Ctx.Body += "    return 1.0;\n";
                             return true;
                         }
                         if (args.Ctx.Pass == Context::Function::PixelDepthOffset)
                         {
                             constexpr size_t pdoPin = 33;
                             if (args.Connected.size() > pdoPin && args.Connected[pdoPin])
                             {
                                 args.Ctx.Body += "    return max(" + Convert(args.In[pdoPin], 1).Code + ", 0.0);\n";
                                 args.Ctx.Outputs2 |= MaterialGraphOutput2PixelDepthOffset;
                             }
                             else
                                 args.Ctx.Body += "    return 0.0;\n";
                             return true;
                         }
                         struct Pin { size_t Index; const char* Field; int Components; uint32_t Bit; const char* Before; const char* After; };
                         const Pin pins[] = {
                             { 0, "baseColor", 3, MaterialGraphOutputBaseColor, "max(", ", float3(0.0))" },
                             { 1, "metallic", 1, MaterialGraphOutputMetallic, "saturate(", ")" },
                             { 2, "roughness", 1, MaterialGraphOutputRoughness, "saturate(", ")" },
                             { 3, "normal", 3, MaterialGraphOutputNormal, "(", ")" }, // (tangent space: the caller normalizes and applies the frame)
                             { 4, "emissive", 3, MaterialGraphOutputEmissive, "max(", ", float3(0.0))" },
                             { 5, "occlusion", 1, MaterialGraphOutputOcclusion, "saturate(", ")" },
                             { 6, "opacity", 1, MaterialGraphOutputOpacity, "saturate(", ")" },
                             { 13, "specular", 1, MaterialGraphOutputSpecular, "saturate(", ")" },
                             { 14, "specularColor", 3, MaterialGraphOutputSpecularColor, "saturate(", ")" },
                             { 15, "clearCoat", 1, MaterialGraphOutputClearCoat, "saturate(", ")" },
                             { 16, "clearCoatRoughness", 1, MaterialGraphOutputClearCoatRoughness, "saturate(", ")" },
                             { 17, "clearCoatNormal", 3, MaterialGraphOutputClearCoatNormal, "(", ")" },
                             { 18, "sheenColor", 3, MaterialGraphOutputSheenColor, "saturate(", ")" },
                             { 19, "sheenRoughness", 1, MaterialGraphOutputSheenRoughness, "saturate(", ")" },
                             { 20, "anisotropy", 1, MaterialGraphOutputAnisotropy, "saturate(", ")" },
                             { 21, "anisotropyDirection", 2, MaterialGraphOutputAnisotropyDirection, "(", ")" },
                             { 22, "transmission", 1, MaterialGraphOutputTransmission, "saturate(", ")" },
                             { 23, "ior", 1, MaterialGraphOutputIOR, "max(", ", 1.0)" },
                             { 24, "thickness", 1, MaterialGraphOutputThickness, "max(", ", 0.0)" },
                             { 25, "attenuationColor", 3, MaterialGraphOutputAttenuationColor, "saturate(", ")" },
                             { 26, "attenuationDistance", 1, MaterialGraphOutputAttenuationDistance, "max(", ", 0.0)" },
                             { 27, "diffuseTransmission", 1, MaterialGraphOutputDiffuseTransmission, "saturate(", ")" },
                             { 28, "iridescence", 1, MaterialGraphOutputIridescence, "saturate(", ")" },
                             { 29, "iridescenceThickness", 1, MaterialGraphOutputIridescenceThickness, "max(", ", 0.0)" },
                             { 30, "dispersion", 1, MaterialGraphOutputDispersion, "max(", ", 0.0)" },
                         };
                         for (const Pin& pin : pins)
                         {
                             if (pin.Index >= args.Connected.size() || !args.Connected[pin.Index])
                                 continue; // (keeps the material's own value)
                             const Value value = Convert(args.In[pin.Index], pin.Components);
                             args.Ctx.Body += std::string("    o.") + pin.Field + " = " + pin.Before + value.Code + pin.After + ";\n";
                             args.Ctx.Outputs |= pin.Bit;
                         }
                         // Extended surface pins (MaterialGraphOutput2, step R1+).
                         constexpr size_t subsurfaceColorPin = 31;
                         if (args.Connected.size() > subsurfaceColorPin && args.Connected[subsurfaceColorPin])
                         {
                             const Value value = Convert(args.In[subsurfaceColorPin], 3);
                             args.Ctx.Body += "    o.subsurfaceColor = saturate(" + value.Code + ");\n";
                             args.Ctx.Outputs2 |= MaterialGraphOutput2SubsurfaceColor;
                         }
                         constexpr size_t aoLightAffectPin = 32;
                         if (args.Connected.size() > aoLightAffectPin && args.Connected[aoLightAffectPin])
                         {
                             const Value value = Convert(args.In[aoLightAffectPin], 1);
                             args.Ctx.Body += "    o.aoLightAffect = saturate(" + value.Code + ");\n";
                             args.Ctx.Outputs2 |= MaterialGraphOutput2AOLightAffect;
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

        // Three functions: surface() from what every Material Output pin but Opacity Mask and World Position Offset needs,
        // opacity_mask() and world_position_offset() from what their pin needs (the geometry passes run them alone; the offset per
        // vertex). An unconnected node computes nothing, and its errors do not count. Each pass starts from the one before's
        // parameters, so a parameter several read has one slot.
        constexpr uint32_t kMaskPin = 7;
        constexpr uint32_t kOffsetPin = 8;
        constexpr uint32_t kLightPin = 9;
        constexpr uint32_t kAmbientPin = 10;
        constexpr uint32_t kVertexNormalPin = 11;
        constexpr uint32_t kVertexColorPin = 12;
        constexpr uint32_t kPdoPin = 33;
        auto pinFunction = [&](uint32_t pin)
        {
            return pin == kMaskPin ? Context::Function::OpacityMask : pin == kOffsetPin ? Context::Function::WorldPositionOffset
                 : pin == kLightPin ? Context::Function::CustomLight : pin == kAmbientPin ? Context::Function::CustomAmbient
                 : (pin == kVertexNormalPin || pin == kVertexColorPin) ? Context::Function::VertexOutputs
                 : pin == kPdoPin ? Context::Function::PixelDepthOffset
                 : Context::Function::Surface;
        };
        auto neededFor = [&](Context::Function function, const std::vector<uint32_t>& extraRoots = {})
        {
            std::vector<bool> needed(compiled.Nodes.size(), false);
            std::vector<uint32_t> stack;
            needed[compiled.OutputNodeIndex] = true;
            const std::vector<CompiledPin>& outputPins = outputNode.Inputs;
            for (uint32_t pin = 0; pin < outputPins.size(); ++pin)
                if (pinFunction(pin) == function && outputPins[pin].Node != kInvalidGraphIndex)
                    stack.push_back(outputPins[pin].Node);
            for (uint32_t root : extraRoots)
                stack.push_back(root);
            while (!stack.empty())
            {
                const uint32_t index = stack.back();
                stack.pop_back();
                if (needed[index])
                    continue;
                needed[index] = true;
                // In Surface / OpacityMask, stop at Vertex Interpolator nodes (their inputs are evaluated in VertexOutputs instead).
                if ((function == Context::Function::Surface || function == Context::Function::OpacityMask) &&
                    compiled.Nodes[index].Type && compiled.Nodes[index].Type->TypeName == "Vertex Interpolator")
                    continue;
                for (const CompiledPin& pin : compiled.Nodes[index].Inputs)
                    if (pin.Node != kInvalidGraphIndex)
                        stack.push_back(pin.Node);
            }
            return needed;
        };

        const std::vector<bool> maskNeeded = neededFor(Context::Function::OpacityMask);
        const std::vector<bool> pdoNeeded = neededFor(Context::Function::PixelDepthOffset);
        const std::vector<bool> surfaceNeeded = neededFor(Context::Function::Surface);
        std::vector<uint32_t> interpolatorRoots;
        std::unordered_map<uint32_t, uint32_t> interpolatorSlots;
        for (uint32_t index = 0; index < compiled.Nodes.size(); ++index)
        {
            if ((!maskNeeded[index] && !pdoNeeded[index] && !surfaceNeeded[index]) || !compiled.Nodes[index].Type ||
                compiled.Nodes[index].Type->TypeName != "Vertex Interpolator")
                continue;
            if (interpolatorSlots.size() >= MaterialGraphMaxInterpolators)
            {
                program.Error = std::format("at most {} Vertex Interpolator nodes per material graph", MaterialGraphMaxInterpolators);
                return program;
            }
            interpolatorSlots[compiled.Nodes[index].SourceNodeId] = static_cast<uint32_t>(interpolatorSlots.size());
            interpolatorRoots.push_back(index);
        }
        const std::vector<bool> vertexNeeded = neededFor(Context::Function::VertexOutputs, interpolatorRoots);

        Context vertexContext;
        vertexContext.Pass = Context::Function::VertexOutputs;
        vertexContext.InterpolatorSlots = interpolatorSlots;
        Context context;
        context.InterpolatorSlots = interpolatorSlots;
        Context maskContext;
        maskContext.Pass = Context::Function::OpacityMask;
        maskContext.InterpolatorSlots = interpolatorSlots;
        Context pdoContext;
        pdoContext.Pass = Context::Function::PixelDepthOffset;
        pdoContext.InterpolatorSlots = interpolatorSlots;
        Context offsetContext;
        offsetContext.Pass = Context::Function::WorldPositionOffset;
        offsetContext.InterpolatorSlots = interpolatorSlots;
        Context lightContext;
        lightContext.Pass = Context::Function::CustomLight;
        Context ambientContext;
        ambientContext.Pass = Context::Function::CustomAmbient;
        // Step 4h: when a graph connects Material Output::Normal (pin 3) AND also reads Pixel Normal in surface(), emit the Normal
        // pin's subgraph first so Pixel Normal sees the world-space normal after the graph's own Normal pin.
        constexpr uint32_t kNormalPin = 3;
        std::vector<bool> normalPrePass;
        if (outputNode.Inputs.size() > kNormalPin && outputNode.Inputs[kNormalPin].Node != kInvalidGraphIndex)
        {
            bool usesPixelNormal = false;
            for (uint32_t index = 0; index < compiled.Nodes.size(); ++index)
                if (surfaceNeeded[index] && compiled.Nodes[index].Type && compiled.Nodes[index].Type->TypeName == "Pixel Normal")
                    usesPixelNormal = true;
            if (usesPixelNormal)
            {
                normalPrePass.assign(compiled.Nodes.size(), false);
                std::vector<uint32_t> stack{ outputNode.Inputs[kNormalPin].Node };
                while (!stack.empty())
                {
                    const uint32_t index = stack.back();
                    stack.pop_back();
                    if (normalPrePass[index])
                        continue;
                    normalPrePass[index] = true;
                    if (compiled.Nodes[index].Type && compiled.Nodes[index].Type->TypeName == "Pixel Normal")
                    {
                        program.Error = "Pixel Normal cannot feed the Normal pin (it reads the normal after the Normal pin)";
                        return program;
                    }
                    if (compiled.Nodes[index].Type && compiled.Nodes[index].Type->TypeName == "Vertex Interpolator")
                        continue;
                    for (const CompiledPin& pin : compiled.Nodes[index].Inputs)
                        if (pin.Node != kInvalidGraphIndex)
                            stack.push_back(pin.Node);
                }
            }
        }

        auto generate = [&](Context& ctx, const std::vector<bool>& needed, const std::vector<bool>* prePass = nullptr) -> bool
        {
        std::vector<std::vector<Value>> outputs(compiled.Nodes.size());
        Context& context = ctx;
        auto emitNode = [&](uint32_t index) -> bool
        {
            const CompiledNode& node = compiled.Nodes[index];
            const auto emitter = Emitters().find(node.Type->TypeName);
            if (emitter == Emitters().end())
            {
                program.Error = std::format("node {} ({}) is not a material node", node.SourceNodeId, node.Type->TypeName);
                return false;
            }
            const std::vector<NodePinDesc> inputPins = ResolveNodePins(*node.Type, node.Properties).Inputs;
            std::vector<Value> in(inputPins.size());
            std::vector<bool> connected(inputPins.size(), false);
            for (size_t pin = 0; pin < inputPins.size(); ++pin)
            {
                const CompiledPin source = pin < node.Inputs.size() ? node.Inputs[pin] : CompiledPin{};
                // (a source this pass does not generate -- the Material Output's pins of the other function -- reads as unconnected)
                if (source.Node != kInvalidGraphIndex && needed[source.Node])
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
                return false;
            }
            // Every output a node gives into a local: a value read by several nodes is computed once.
            for (Value& value : outputs[index])
                if (!value.Code.empty() && value.Code.find_first_of("(+-*/") != std::string::npos)
                    value = context.Store(value);
            return true;
        };
        if (prePass && !prePass->empty())
        {
            for (uint32_t index = 0; index < compiled.Nodes.size(); ++index)
                if ((*prePass)[index] && !emitNode(index))
                    return false;
            const CompiledPin normalSrc = outputNode.Inputs[kNormalPin];
            const Value normalVal = Convert(outputs[normalSrc.Node].at(normalSrc.OutputSlot), 3);
            context.Body += "    const float3 _pixelNormal = material_graph_world_normal(" + normalVal.Code + ", i.normal, i.tangent.xyz, i.tangent.w);\n";
            context.PixelNormalExpr = "_pixelNormal";
        }
        for (uint32_t index = 0; index < compiled.Nodes.size(); ++index)
        {
            if (!needed[index] || (prePass && !prePass->empty() && (*prePass)[index]))
                continue;
            if (!emitNode(index))
                return false;
        }
        return true;
        };

        // The mask first: its parameters and textures take the first slots, so its code (MaskCode, which every ray tracing pass's variant
        // compiles: MaterialGraphMasks.slang) depends on the mask's own nodes alone -- a parameter added to or removed from the surface
        // used to move the mask's slot numbers and rebuild every ray tracing variant for an unchanged mask.
        auto continueFrom = [](Context& next, const Context& previous)
        {
            next.Parameters = previous.Parameters;
            next.ParameterSlots = previous.ParameterSlots;
            next.TextureSlots = previous.TextureSlots;
            next.InterpolatorComponents = previous.InterpolatorComponents;
        };
        // VertexOutputs runs before Mask / Surface so Vertex Interpolator component counts are known when pixel passes read them.
        if (!generate(vertexContext, vertexNeeded))
            return program;
        continueFrom(maskContext, vertexContext);
        if (!generate(maskContext, maskNeeded))
            return program;
        continueFrom(context, maskContext);
        if (!generate(context, surfaceNeeded, normalPrePass.empty() ? nullptr : &normalPrePass))
            return program;
        continueFrom(pdoContext, context);
        if (!generate(pdoContext, pdoNeeded))
            return program;
        continueFrom(offsetContext, pdoContext);
        if (!generate(offsetContext, neededFor(Context::Function::WorldPositionOffset)))
            return program;
        // The lighting last: its parameters after the others' (a lighting edit moves no slot the other functions use).
        continueFrom(lightContext, offsetContext);
        if (!generate(lightContext, neededFor(Context::Function::CustomLight)))
            return program;
        continueFrom(ambientContext, lightContext);
        if (!generate(ambientContext, neededFor(Context::Function::CustomAmbient)))
            return program;
        Context& lastContext = ambientContext;

        program.Valid = true;
        program.Parameters = std::move(lastContext.Parameters);
        // Listed by name (the Material Editor and the graph editor show them in this order; each keeps its Slot).
        std::stable_sort(program.Parameters.begin(), program.Parameters.end(), [](const MaterialGraphParameter& a, const MaterialGraphParameter& b)
        {
            return std::lexicographical_compare(a.Name.begin(), a.Name.end(), b.Name.begin(), b.Name.end(),
                                                [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
        });
        program.ParameterSlots = lastContext.ParameterSlots;
        program.TextureSlots = lastContext.TextureSlots;
        const uint32_t geometryOutputs = context.Outputs | maskContext.Outputs | offsetContext.Outputs | vertexContext.Outputs;
        program.Outputs = geometryOutputs | lightContext.Outputs | ambientContext.Outputs;
        program.Outputs2 = context.Outputs2 | pdoContext.Outputs2;
        if (const NodeGraphValue* displacement = Property(outputNode, "Max World Position Offset Displacement"))
            if (const float* value = std::get_if<float>(displacement))
                program.MaxDisplacement = std::max(*value, 0.0f);
        if ((maskContext.Outputs & MaterialGraphOutputOpacityMask) != 0)
            program.MaskCode = std::format("float opacity_mask(MaterialGraphInputs i)\n{{\n{}}}\n", maskContext.Body);
        if ((vertexContext.Outputs & (MaterialGraphOutputVertexNormal | MaterialGraphOutputVertexColor | MaterialGraphOutputVertexInterpolators)) != 0)
            program.VertexCode = std::format("void vertex_outputs(MaterialGraphInputs i, inout MaterialGraphVertexOutputs vo)\n{{\n{}}}\n", vertexContext.Body);
        program.SurfaceCode = std::format("void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o)\n{{\n{}}}\n", context.Body);
        program.Code = std::format("static const uint Outputs = {}u;\n\nvoid surface(MaterialGraphInputs i, inout MaterialGraphOutputs o)\n{{\n{}}}\n\n"
                                   "float opacity_mask(MaterialGraphInputs i)\n{{\n{}}}\n\n"
                                   "float pixel_depth_offset(MaterialGraphInputs i)\n{{\n{}}}\n\n"
                                   "float3 world_position_offset(MaterialGraphInputs i)\n{{\n{}}}\n\n"
                                   "void vertex_outputs(MaterialGraphInputs i, inout MaterialGraphVertexOutputs vo)\n{{\n{}}}\n",
                                   geometryOutputs, context.Body, maskContext.Body, pdoContext.Body, offsetContext.Body, vertexContext.Body);
        // The graph's lighting for a Custom Lit material (Custom Light / Custom Ambient): its inputs from the Custom Lit surface
        // (material_graph_lit_inputs, generated into CustomLitShaders.slang with it), so the graph's other nodes read there too.
        if ((program.Outputs & (MaterialGraphOutputCustomLight | MaterialGraphOutputCustomAmbient)) != 0)
            program.LightCode = std::format("float3 light(CustomLitSurface s, CustomLitLight l)\n{{\n    const MaterialGraphInputs i = material_graph_lit_inputs(s);\n{}}}\n\n"
                                            "float3 ambient(CustomLitSurface s, CustomLitAmbient a)\n{{\n    const MaterialGraphInputs i = material_graph_lit_inputs(s);\n{}}}\n",
                                            lightContext.Body, ambientContext.Body);
        return program;
    }
}
