#!/usr/bin/env python3
"""Generate typed, owning Vulkan command capture from the installed registry.

Unknown schemas fail capture, while the original command can still be forwarded.
The JSON manifest is deliberately part of the output: adding a registry command
must never silently turn it into a shallow-copy replay implementation.
"""
import argparse
import ast
import hashlib
import json
import re
from pathlib import Path
import xml.etree.ElementTree as ET


def vulkan(node):
    return "vulkan" in node.get("api", "vulkan").split(",")


def declaration(node):
    return "".join(node.itertext()).split("<comment>")[0].strip()


def name_of(node):
    return node.findtext("name") or node.get("name")


class Generator:
    TEMPLATE_COMMANDS = {
        "vkCmdPushDescriptorSetWithTemplate", "vkCmdPushDescriptorSetWithTemplateKHR",
        "vkCmdPushDescriptorSetWithTemplate2", "vkCmdPushDescriptorSetWithTemplate2KHR",
    }
    INACTIVE_DYNAMIC_INHERITANCE = {
        "VkCommandBufferInheritanceRenderingInfo",
        "VkAttachmentSampleCountInfoAMD",
        "VkRenderingAttachmentLocationInfo",
    }
    UNMODELED_INACTIVE_INHERITANCE = {
        "VkRenderingInputAttachmentIndexInfo",
        "VkCustomResolveCreateInfoEXT",
    }
    # noautovalidity explicitly means that XML alone does not describe the
    # pointer contract. These are reviewed semantic rules, not default lengths.
    # Unknown noautovalidity pointers without an extent must fail closed.
    MANUAL_POINTER_RULES = {
        "VkCommandBufferBeginInfo.pInheritanceInfo": "one structure for secondary buffers; caller sanitizes ignored primary-buffer inheritance",
        "VkCommandBufferInheritanceViewportScissorInfoNV.pViewportDepths": "viewportScissor2D ? viewportDepthCount : 0; never dereference when disabled",
        "vkCmdClearColorImage.pColor": "one POD VkClearColorValue union",
        "vkCmdPushDescriptorSetWithTemplate.pData": "normalize with immutable integration template metadata; otherwise unsupported",
        "vkCmdPushDescriptorSetWithTemplateKHR.pData": "normalize with immutable integration template metadata; otherwise unsupported",
        "VkPushDescriptorSetWithTemplateInfo.pData": "normalize with immutable integration template metadata; otherwise unsupported",
        "vkCmdSetCheckpointNV.pCheckpointMarker": "opaque identity token; never dereference",
        "VkDeviceOrHostAddressKHR.hostAddress": "vkCmd operations interpret the union as deviceAddress bits; never dereference hostAddress",
        "VkDeviceOrHostAddressConstKHR.hostAddress": "vkCmd operations interpret the union as deviceAddress bits; never dereference hostAddress",
    }

    def __init__(self, registry, header):
        self.registry, self.header = registry, header
        self.root = ET.parse(registry).getroot()
        self.header_text = header.read_text(encoding="utf-8")
        self.types = {name_of(t): t for t in self.root.findall("./types/type") if vulkan(t)}
        self.available = set(re.findall(r"\b(Vk\w+)\b", self.header_text))
        self.available_structs = set(re.findall(r"typedef\s+(?:struct|union)\s+(Vk\w+)\s*\{", self.header_text))
        available_commands = set(re.findall(r"\bPFN_(vkCmd\w+)\b", self.header_text))
        self.all_commands = {}
        self.commands = []
        self.header_missing_commands = []
        for c in self.root.findall("./commands/command"):
            if vulkan(c):
                self.all_commands[c.get("name") or c.findtext("./proto/name")] = c
        for n, c in self.all_commands.items():
            target = c
            while target.get("alias"):
                target = self.all_commands[target.get("alias")]
            params = [p for p in target.findall("param") if vulkan(p)]
            if n and n.startswith("vkCmd") and params and params[0].findtext("type") == "VkCommandBuffer":
                if n in available_commands:
                    self.commands.append((n, target, params))
                else:
                    self.header_missing_commands.append((n, target, params))
        self.commands.sort()
        self.header_missing_commands.sort()
        self.reachable = set()
        self.walked = set()
        self.struct_reasons = {}
        self.command_reasons = {}
        self.selected_unions = set()
        self.count = 0
        self.abi_declarations = {}
        self.abi_types = {}
        self.abi_rejections = {}

    def forwarding_type(self, n, pointer):
        """Prove just the call ABI; never inspect an unavailable pointee.

        Private aliases avoid colliding with optional beta/platform headers.
        Struct/union tags can be forward declared without knowing their layout.
        By-value aggregate layouts cannot be guessed and remain rejected.
        """
        if n in self.available or n in ("void", "char", "float", "double", "int", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int32_t", "int64_t", "size_t"):
            return n
        if n in self.abi_types:
            if self.category(n) in ("struct", "union") and not pointer:
                raise ValueError("unavailable by-value aggregate ABI " + n)
            return self.abi_types[n]
        canonical = self.canonical(n)
        node = self.types.get(canonical)
        if node is None:
            raise ValueError("unknown external ABI type " + n)
        category = node.get("category")
        if category in ("struct", "union"):
            if not pointer:
                raise ValueError("unavailable by-value aggregate ABI " + n)
            # Vulkan uses the same identifier for the typedef and aggregate tag.
            # Repeating an identical typedef is valid C11 when a beta header also
            # declares the type, while the private PFN name never collides.
            self.abi_declarations[n] = "typedef " + category + " " + canonical + " " + n + ";"
            self.abi_types[n] = n
            return n
        private = "VKRT_ABI_" + n
        if category == "handle":
            spelling = "".join(node.itertext())
            macro = "VK_DEFINE_NON_DISPATCHABLE_HANDLE" if "VK_DEFINE_NON_DISPATCHABLE_HANDLE" in spelling else "VK_DEFINE_HANDLE" if "VK_DEFINE_HANDLE" in spelling else None
            if not macro:
                raise ValueError("unrecognized handle ABI " + n)
            self.abi_declarations[n] = macro + "(" + private + ")"
        elif category in ("basetype", "bitmask"):
            underlying = node.findtext("type")
            if not underlying:
                raise ValueError("unrecognized scalar ABI " + n)
            base = self.forwarding_type(underlying, False)
            self.abi_declarations[n] = "typedef " + base + " " + private + ";"
        elif category == "enum":
            values = self.root.find("./enums[@name='" + canonical + "']")
            if values is None or values.get("bitwidth", "32") not in ("32", "64"):
                raise ValueError("unrecognized enum ABI " + n)
            base = "uint64_t" if values.get("bitwidth") == "64" else "int32_t"
            self.abi_declarations[n] = "typedef " + base + " " + private + ";"
        else:
            raise ValueError("unsupported ABI category " + str(category) + " for " + n)
        self.abi_types[n] = private
        return private

    def forwarding_signature(self, c, params):
        ret = c.findtext("./proto/type")
        if ret not in ("void", "VkResult"):
            raise ValueError("unrecognized return ABI " + ret)
        declarations = []
        for p in params:
            t, _, decl, depth, dims = self.shape(p)
            replacement = self.forwarding_type(t, bool(depth or dims))
            declarations.append(re.sub(r"\b" + re.escape(t) + r"\b", replacement, decl, count=1))
        return ret, ", ".join(declarations)

    def forwarding_command(self, n, c, params, ret, signature):
        args = [name_of(p) for p in params]
        lines = ["typedef " + ret + " (VKAPI_PTR *VKRT_PFN_" + n + ")(" + signature + ");",
                 "static VKAPI_ATTR " + ret + " VKAPI_CALL replay_record_" + n + "(" + signature + ") {",
                 "    struct vkrt_replay_context *context = vkrt_replay_lookup_context(" + args[0] + ");",
                 "    if (context && context->journal) {",
                 "        struct replay_arena arena = replay_capture_begin(context);",
                 "        replay_unsupported(&arena);",
                 "        replay_capture_end(context, \"" + n + "\", NULL, NULL, NULL, &arena);",
                 "    }",
                 "    VKRT_PFN_" + n + " proc = context && context->resolve ? (VKRT_PFN_" + n + ")context->resolve(context->user, \"" + n + "\") : NULL;"]
        if ret == "void":
            lines.append("    if (proc) proc(" + ", ".join(args) + ");")
        else:
            lines.append("    return proc ? proc(" + ", ".join(args) + ") : VK_ERROR_FEATURE_NOT_PRESENT;")
        lines.append("}")
        return lines

    def canonical(self, n):
        seen = set()
        while n in self.types and self.types[n].get("alias"):
            if n in seen:
                raise ValueError("type alias cycle")
            seen.add(n)
            n = self.types[n].get("alias")
        return n

    def category(self, n):
        t = self.types.get(self.canonical(n))
        return t.get("category") if t is not None else None

    def members(self, n):
        return [m for m in self.types[self.canonical(n)].findall("member") if vulkan(m)]

    def shape(self, m):
        # Comments are not part of C declarations (itertext includes their text).
        before = m.text or ""
        ty = m.find("type")
        nm = m.find("name")
        middle = ty.tail or ""
        tail = nm.tail or ""
        after_name = False
        for child in m:
            if child is nm:
                after_name = True
            elif after_name:
                if child.tag == "comment":
                    break
                tail += "".join(child.itertext()) + (child.tail or "")
        return ty.text, nm.text, before + ty.text + middle + nm.text + tail, middle.count("*"), re.findall(r"\[([^]]+)\]", tail)

    def pod_union(self, n, seen=None):
        n = self.canonical(n)
        if n in ("VkDeviceOrHostAddressKHR", "VkDeviceOrHostAddressConstKHR"):
            # vkCmd AS operations use deviceAddress; these bits are not host memory.
            return True
        seen = set() if seen is None else seen
        if n in seen:
            return True
        seen.add(n)
        for m in self.members(n):
            t, _, _, depth, _ = self.shape(m)
            if depth:
                return False
            if self.category(t) in ("struct", "union") and not self.pod_union(t, seen):
                return False
        return True

    def add_type(self, n):
        n = self.canonical(n)
        if n in self.walked or n not in self.available_structs or self.category(n) not in ("struct", "union"):
            return
        self.walked.add(n)
        if self.category(n) == "struct":
            self.reachable.add(n)
        for m in self.members(n):
            self.add_type(m.findtext("type"))
        # Follow legal pNext descendants, including descendants of descendants.
        for ext, node in self.types.items():
            if n in node.get("structextends", "").split(","):
                self.add_type(ext)

    def ident(self):
        self.count += 1
        return "v" + str(self.count)

    def expression(self, expr, env):
        expr = expr.strip().replace("::", ".").replace("->", ".")
        parsed = ast.parse(expr, mode="eval").body

        def render(node):
            if isinstance(node, ast.Constant) and isinstance(node.value, int):
                return str(node.value)
            if isinstance(node, ast.Name):
                if node.id in env:
                    return "replay_size_value(arena, (uint64_t)(" + env[node.id] + "))"
                if node.id.startswith("VK_") or node.id.isupper():
                    return "replay_size_value(arena, (uint64_t)(" + node.id + "))"
                raise ValueError("unknown length identifier " + node.id)
            if isinstance(node, ast.Attribute):
                # Registry lengths select members through pointer parameters.
                def path(x):
                    if isinstance(x, ast.Name) and x.id in env:
                        return "(" + env[x.id] + ")"
                    if isinstance(x, ast.Attribute):
                        return "(" + path(x.value) + "->" + x.attr + ")"
                    raise ValueError("unhandled member length")
                return "replay_size_value(arena, (uint64_t)" + path(node) + ")"
            if isinstance(node, ast.BinOp):
                op = {ast.Add: "add", ast.Sub: "sub", ast.Mult: "mul", ast.Div: "div", ast.FloorDiv: "div"}.get(type(node.op))
                if not op:
                    raise ValueError("unhandled length operator")
                return "replay_size_" + op + "(arena, " + render(node.left) + ", " + render(node.right) + ")"
            raise ValueError("unhandled length " + expr)
        return render(parsed)

    def length(self, m, env, dims):
        if dims:
            count = "1"
            for d in dims:
                count = "replay_size_mul(arena, " + count + ", " + self.expression(d, env) + ")"
            return count
        value = m.get("altlen") or m.get("len")
        if not value:
            return "1"
        value = value.split(",")[0]
        if value == "null-terminated":
            return "string"
        return self.expression(value, env)

    def clone_value(self, t, dst, src, lines, indent):
        category = self.category(t)
        t0 = self.canonical(t)
        if t0 == "VkPipelineLayout":
            lines.append(indent + dst + " = replay_layout(arena, " + src + ");")
        elif category == "struct":
            if t0 in self.reachable:
                lines.append(indent + "replay_clone_" + t0 + "(arena, &" + dst + ", &" + src + ");")
            else:
                lines.append(indent + "replay_unsupported(arena);")
        elif category == "union" and not self.pod_union(t):
            lines.append(indent + "replay_unsupported(arena);")
        else:
            lines.append(indent + dst + " = " + src + ";")

    def clone_member(self, m, dst, src, env, lines, indent="    ", owner=""):
        t, n, decl, depth, dims = self.shape(m)
        if n == "pNext":
            lines.append(indent + dst + " = replay_clone_chain(arena, " + src + ");")
            return
        if owner == "VkCommandBufferInheritanceViewportScissorInfoNV" and n == "pViewportDepths":
            lines.extend([indent + dst + " = NULL;",
                          indent + "if (src->viewportScissor2D && src->viewportDepthCount) {",
                          indent + "    " + dst + " = replay_copy(arena, " + src + ", src->viewportDepthCount, sizeof(*" + src + "));",
                          indent + "}"])
            return
        if depth and m.get("noautovalidity") == "true" and not m.get("len") and not m.get("altlen") and not dims:
            if owner + "." + n not in self.MANUAL_POINTER_RULES:
                raise ValueError("unmodeled noautovalidity pointer contract")
        if owner == "VkAccelerationStructureGeometryKHR" and n == "geometry":
            lines.extend([indent + "switch (src->geometryType) {",
                          indent + "case VK_GEOMETRY_TYPE_TRIANGLES_KHR: replay_clone_VkAccelerationStructureGeometryTrianglesDataKHR(arena, &dst->geometry.triangles, &src->geometry.triangles); break;",
                          indent + "case VK_GEOMETRY_TYPE_AABBS_KHR: replay_clone_VkAccelerationStructureGeometryAabbsDataKHR(arena, &dst->geometry.aabbs, &src->geometry.aabbs); break;",
                          indent + "case VK_GEOMETRY_TYPE_INSTANCES_KHR: replay_clone_VkAccelerationStructureGeometryInstancesDataKHR(arena, &dst->geometry.instances, &src->geometry.instances); break;",
                          indent + "default: replay_unsupported(arena); break;", indent + "}"])
            return
        if not depth and not dims and self.category(t) == "union" and not self.pod_union(t) and m.get("selector"):
            selector = m.get("selector")
            union_members = self.members(t)
            if selector not in env or any(not u.get("selection") for u in union_members):
                raise ValueError("union selector schema is incomplete")
            self.selected_unions.add(self.canonical(t))
            lines.append(indent + "switch (" + env[selector] + ") {")
            for u in union_members:
                for value in u.get("selection").split(","):
                    lines.append(indent + "case " + value + ":")
                lines.append(indent + "{")
                self.clone_member(u, dst + "." + name_of(u), src + "." + name_of(u), env, lines, indent + "    ", owner=self.canonical(t))
                lines.extend([indent + "    break;", indent + "}"])
            lines.extend([indent + "default: replay_unsupported(arena); break;", indent + "}"])
            return
        if not depth and not dims:
            self.clone_value(t, dst, src, lines, indent)
            return
        if dims and not depth and owner.startswith("Vk"):
            # Embedded arrays are owned by the containing structure. Recursively
            # normalize handles/structs rather than leaving pointer subfields.
            idx = self.ident()
            count = self.length(m, env, dims)
            lines.append(indent + "for (size_t " + idx + " = 0; " + idx + " < " + count + " && arena->status == VK_SUCCESS; ++" + idx + ") {")
            self.clone_value(t, "((" + t + " *)&" + dst + ")[" + idx + "]", "((const " + t + " *)&" + src + ")[" + idx + "]", lines, indent + "    ")
            lines.append(indent + "}")
            return
        if not depth and dims:
            depth = 1  # C function array parameters decay to pointers.
        if owner == "vkCmdSetCheckpointNV" and n == "pCheckpointMarker":
            lines.append(indent + dst + " = " + src + "; /* opaque identity token */")
            return
        if owner.startswith("vkCmd") and depth and "const" not in (m.text or ""):
            raise ValueError("recording-time writable output requires semantic replay")
        if m.get("stride"):
            if depth != 1 or self.category(t) not in ("struct", "union") or not self.pod_union(t):
                raise ValueError("strided non-POD array requires semantic marshal")
            count = self.length(m, env, dims)
            stride = self.expression(m.get("stride"), env)
            countvar, stridevar, buf, i = self.ident(), self.ident(), self.ident(), self.ident()
            lines.extend([indent + dst + " = NULL;", indent + "if (" + src + " && arena->status == VK_SUCCESS) {",
                          indent + "    size_t " + countvar + " = " + count + ";",
                          indent + "    size_t " + stridevar + " = " + stride + ";",
                          indent + "    if (" + countvar + ") {",
                          indent + "        size_t bytes = replay_size_add(arena, replay_size_mul(arena, replay_size_sub(arena, " + countvar + ", 1), " + stridevar + "), sizeof(" + t + "));",
                          indent + "        unsigned char *" + buf + " = replay_alloc(arena, bytes, 1);",
                          indent + "        " + dst + " = (const " + t + " *)" + buf + ";",
                          indent + "        if (" + buf + ") for (size_t " + i + " = 0; " + i + " < " + countvar + " && arena->status == VK_SUCCESS; ++" + i + ") {"])
            # Gaps are not input memory. Copy only the active records, preserving
            # stride without ever reading padding or uncommitted address gaps.
            self.clone_value(t, "*(" + t + " *)(" + buf + " + " + i + " * " + stridevar + ")", "*(const " + t + " *)((const unsigned char *)" + src + " + " + i + " * " + stridevar + ")", lines, indent + "            ")
            lines.extend([indent + "        }", indent + "    }", indent + "}"])
            return
        if depth > 2:
            raise ValueError("pointer depth greater than two")
        if t == "void" and not m.get("len") and not m.get("altlen") and not dims:
            raise ValueError("opaque void pointer has no byte length")
        if self.category(t) == "funcpointer":
            raise ValueError("function pointer input requires semantic marshal")
        if t not in self.available and t not in ("void", "char", "float", "double", "int", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int32_t", "int64_t", "size_t"):
            raise ValueError("external pointee schema " + t)
        count = self.length(m, env, dims)
        lines.append(indent + dst + " = NULL;")
        lines.append(indent + "if (" + src + " && arena->status == VK_SUCCESS) {")
        inner = indent + "    "
        countvar = self.ident()
        if count == "string":
            if t != "char" or depth != 1:
                raise ValueError("non-character null-terminated data")
            count = "replay_size_add(arena, strlen(" + src + "), 1)"
        lines.append(inner + "size_t " + countvar + " = " + count + ";")
        if depth == 2:
            if t == "void":
                raise ValueError("opaque pointer array has no pointee schema")
            arr, i = self.ident(), self.ident()
            lines.append(inner + t + " **" + arr + " = replay_alloc(arena, " + countvar + ", sizeof(*" + arr + "));")
            lines.append(inner + dst + " = (void *)" + arr + ";")
            lines.append(inner + "if (" + arr + ") for (size_t " + i + " = 0; " + i + " < " + countvar + " && arena->status == VK_SUCCESS; ++" + i + ") {")
            row = "1"
            if owner in ("vkCmdBuildAccelerationStructuresKHR", "vkCmdBuildAccelerationStructuresIndirectKHR") and n in ("ppBuildRangeInfos", "ppMaxPrimitiveCounts"):
                row = "((size_t)(" + env["pInfos"] + ")[" + i + "].geometryCount)"
            elif m.get("len") and len(m.get("len").split(",")) > 1:
                rowpart = m.get("len").split(",")[1]
                if rowpart == "null-terminated" and t == "char":
                    row = "replay_size_add(arena, strlen(" + src + "[" + i + "]), 1)"
                else:
                    row = self.expression(rowpart, env)
            rowvar, j = self.ident(), self.ident()
            lines.append(inner + "    " + arr + "[" + i + "] = NULL;")
            lines.append(inner + "    if (" + src + "[" + i + "]) {")
            lines.append(inner + "        size_t " + rowvar + " = " + row + ";")
            lines.append(inner + "        " + arr + "[" + i + "] = replay_alloc(arena, " + rowvar + ", sizeof(" + t + "));")
            lines.append(inner + "        if (" + arr + "[" + i + "]) for (size_t " + j + " = 0; " + j + " < " + rowvar + " && arena->status == VK_SUCCESS; ++" + j + ") {")
            self.clone_value(t, arr + "[" + i + "][" + j + "]", src + "[" + i + "][" + j + "]", lines, inner + "            ")
            lines.extend([inner + "        }", inner + "    }", inner + "}"])
        elif t == "void":
            lines.append(inner + dst + " = replay_copy(arena, " + src + ", " + countvar + ", 1);")
        else:
            arr, i = self.ident(), self.ident()
            lines.append(inner + t + " *" + arr + " = replay_alloc(arena, " + countvar + ", sizeof(*" + arr + "));")
            lines.append(inner + dst + " = " + arr + ";")
            lines.append(inner + "if (" + arr + ") for (size_t " + i + " = 0; " + i + " < " + countvar + " && arena->status == VK_SUCCESS; ++" + i + ") {")
            self.clone_value(t, arr + "[" + i + "]", src + "[" + i + "]", lines, inner + "    ")
            lines.append(inner + "}")
        lines.append(indent + "}")

    def struct_clone(self, n):
        lines = ["static inline void replay_clone_" + n + "(struct replay_arena *arena, " + n + " *dst, const " + n + " *src) {", "    (void)arena;"]
        if n in self.UNMODELED_INACTIVE_INHERITANCE:
            lines.extend(["    if (arena->in_inheritance && !arena->dynamic_rendering_inheritance) {",
                          "        replay_unsupported(arena); return;", "    }"])
        lines.append("    *dst = *src;")
        if n == "VkCommandBufferInheritanceInfo":
            lines.extend(["    bool previous_inheritance = arena->in_inheritance;",
                          "    bool previous_dynamic = arena->dynamic_rendering_inheritance;",
                          "    arena->in_inheritance = true;",
                          "    arena->dynamic_rendering_inheritance = arena->has_begin_context &&",
                          "        (arena->begin_flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) && src->renderPass == VK_NULL_HANDLE;"])
        if n == "VkPipelineLayoutCreateInfo":
            lines.extend(["    /* Inline DSL handles need their own retained semantic representation. */", "    replay_unsupported(arena);", "}"])
            self.struct_reasons[n] = ["inline descriptor set layouts require retained semantic representation"]
            return lines
        members = self.members(n)
        env = {name_of(m): "src->" + name_of(m) for m in members}
        reasons = (["inactive non-dynamic inheritance context requires a separate semantic rule"]
                   if n in self.UNMODELED_INACTIVE_INHERITANCE else [])
        if n == "VkWriteDescriptorSet":
            members = [m for m in members if name_of(m) not in ("pImageInfo", "pBufferInfo", "pTexelBufferView")]
        for m in members:
            block = []
            try:
                self.clone_member(m, "dst->" + name_of(m), "src->" + name_of(m), env, block, owner=n)
            except (ValueError, SyntaxError) as exc:
                reasons.append(name_of(m) + ": " + str(exc))
                _, _, _, depth, _ = self.shape(m)
                if depth:
                    if str(exc) == "unmodeled noautovalidity pointer contract":
                        block = ["    dst->" + name_of(m) + " = NULL;", "    replay_unsupported(arena);"]
                    else:
                        block = ["    dst->" + name_of(m) + " = NULL;", "    if (src->" + name_of(m) + ") replay_unsupported(arena);"]
                else:
                    block = ["    replay_unsupported(arena);"]
            lines.extend(block)
        if n == "VkWriteDescriptorSet":
            lines.extend(["    dst->pImageInfo = NULL; dst->pBufferInfo = NULL; dst->pTexelBufferView = NULL;",
                          "    switch (src->descriptorType) {",
                          "    case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:",
                          "    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:",
                          "        dst->pImageInfo = replay_copy(arena, src->pImageInfo, src->descriptorCount, sizeof(*src->pImageInfo)); break;",
                          "    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:",
                          "    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:",
                          "        dst->pBufferInfo = replay_copy(arena, src->pBufferInfo, src->descriptorCount, sizeof(*src->pBufferInfo)); break;",
                          "    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:",
                          "        dst->pTexelBufferView = replay_copy(arena, src->pTexelBufferView, src->descriptorCount, sizeof(*src->pTexelBufferView)); break;",
                          "    case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:",
                          "    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV: break;",
                          "    default: replay_unsupported(arena); break;", "    }"])
        if n == "VkCommandBufferInheritanceInfo":
            lines.extend(["    arena->in_inheritance = previous_inheritance;",
                          "    arena->dynamic_rendering_inheritance = previous_dynamic;"])
        lines.append("}")
        if reasons:
            self.struct_reasons[n] = reasons
        return lines

    def saved_decl(self, m):
        t, n, decl, depth, dims = self.shape(m)
        if dims:
            return "const " + t + " *" + n + ";"
        # Strip leading const only for the field itself? Pointer pointee const
        # is retained; scalar const fields would prevent normal assignment.
        if not depth:
            return t + " " + n + ";"
        return decl + ";"

    def metadata(self, c, params):
        """Reviewed stage facts; registry state-only is distinct from work.

        Never classify a new action by an English prefix or its native queue
        capabilities. Unknown action/synchronization semantics fail the metadata
        query while leaving capture/replay untouched.
        """
        n = c.findtext("./proto/name")
        names = {name_of(p) for p in params[1:]}
        stage = "VK_PIPELINE_STAGE_2_"
        flag = "VKRT_REPLAY_"
        lines = []

        def kind(k, logical=None, physical=None, flags=()):
            lines.append("    metadata->kind = VKRT_REPLAY_" + k + ";")
            if logical:
                lines.append("    metadata->logical_stages = " + logical + ";")
            if physical:
                lines.append("    metadata->physical_stages = " + physical + ";")
            if flags:
                lines.append("    metadata->flags = " + " | ".join(flag + f for f in flags) + ";")

        def assign(path, expr):
            lines.append("    metadata->data." + path + " = " + expr + ";")

        def source(path, member):
            assign(path, "saved->" + member)

        def required(member):
            lines.append("    if (!saved->" + member + ") return VK_ERROR_FEATURE_NOT_PRESENT;")

        if n == "vkCmdPipelineBarrier" or n == "vkCmdWaitEvents":
            kind("BARRIER" if n == "vkCmdPipelineBarrier" else "EVENT_WAIT", flags=("RAW_SCOPES",))
            source("sync.src_stage_mask", "srcStageMask")
            source("sync.dst_stage_mask", "dstStageMask")
            if "dependencyFlags" in names:
                source("sync.dependency_flags", "dependencyFlags")
            for field, member in (("memory_barrier_count", "memoryBarrierCount"),
                                  ("buffer_barrier_count", "bufferMemoryBarrierCount"),
                                  ("image_barrier_count", "imageMemoryBarrierCount"),
                                  ("memory_barriers", "pMemoryBarriers"),
                                  ("buffer_barriers", "pBufferMemoryBarriers"),
                                  ("image_barriers", "pImageMemoryBarriers")):
                source("sync." + field, member)
            if n == "vkCmdWaitEvents":
                source("sync.event_count", "eventCount")
                source("sync.events", "pEvents")
        elif n in ("vkCmdPipelineBarrier2", "vkCmdSetEvent2", "vkCmdWaitEvents2"):
            kind({"vkCmdPipelineBarrier2": "BARRIER", "vkCmdSetEvent2": "EVENT_SET", "vkCmdWaitEvents2": "EVENT_WAIT"}[n],
                 flags=("RAW_SCOPES",))
            assign("sync.synchronization2", "true")
            if n == "vkCmdWaitEvents2":
                source("sync.event_count", "eventCount")
                source("sync.events", "pEvents")
                source("sync.dependency_info_count", "eventCount")
                source("sync.dependency_infos", "pDependencyInfos")
            else:
                assign("sync.dependency_info_count", "1")
                source("sync.dependency_infos", "pDependencyInfo")
                if n == "vkCmdSetEvent2":
                    source("sync.event", "event")
        elif n in ("vkCmdSetEvent", "vkCmdResetEvent", "vkCmdResetEvent2"):
            kind("EVENT_SET" if n == "vkCmdSetEvent" else "EVENT_RESET", flags=("RAW_SCOPES",))
            source("sync.event", "event")
            source("sync.src_stage_mask", "stageMask")
            if n == "vkCmdResetEvent2":
                assign("sync.synchronization2", "true")
        elif n in ("vkCmdBeginQuery", "vkCmdBeginQueryIndexedEXT", "vkCmdEndQuery", "vkCmdEndQueryIndexedEXT",
                   "vkCmdResetQueryPool", "vkCmdCopyQueryPoolResults", "vkCmdWriteTimestamp", "vkCmdWriteTimestamp2"):
            query_kinds = {"vkCmdBeginQuery": "QUERY_BEGIN", "vkCmdBeginQueryIndexedEXT": "QUERY_BEGIN",
                           "vkCmdEndQuery": "QUERY_END", "vkCmdEndQueryIndexedEXT": "QUERY_END",
                           "vkCmdResetQueryPool": "QUERY_RESET", "vkCmdCopyQueryPoolResults": "QUERY_COPY",
                           "vkCmdWriteTimestamp": "QUERY_TIMESTAMP", "vkCmdWriteTimestamp2": "QUERY_TIMESTAMP"}
            flags = ["QUERY_POOL_CONTEXT"]
            if "stage" in names:
                flags.append("RAW_SCOPES")
            copy = n == "vkCmdCopyQueryPoolResults"
            kind(query_kinds[n], stage + "COPY_BIT" if copy else None, stage + "COPY_BIT" if copy else None, flags)
            source("query.pool", "queryPool")
            source("query.first", "firstQuery" if "firstQuery" in names else "query")
            if "queryCount" in names:
                source("query.count", "queryCount")
            else:
                assign("query.count", "1")
            if "index" in names:
                source("query.index", "index")
            if "flags" in names:
                source("query.result_flags" if copy else "query.control_flags", "flags")
            if "stage" in names:
                source("query.stage_mask", "stage")
            if copy:
                source("query.destination", "dstBuffer")
                source("query.offset", "dstOffset")
                source("query.stride", "stride")
        elif n == "vkCmdWriteAccelerationStructuresPropertiesKHR":
            kind("AS_PROPERTY", physical=stage + "COMPUTE_SHADER_BIT", flags=("LOWERED", "AS_COPY_CONTEXT"))
            source("query.pool", "queryPool")
            source("query.first", "firstQuery")
            source("query.count", "accelerationStructureCount")
            source("query.type", "queryType")
            source("query.acceleration_structures", "pAccelerationStructures")
        elif n in ("vkCmdBuildAccelerationStructuresKHR", "vkCmdBuildAccelerationStructuresIndirectKHR"):
            indirect = n.endswith("IndirectKHR")
            # AS indirect arguments execute at AS BUILD, not DRAW_INDIRECT.
            kind("AS_BUILD", stage + "ACCELERATION_STRUCTURE_BUILD_BIT_KHR", stage + "COMPUTE_SHADER_BIT",
                 ("LOWERED", "INDIRECT") if indirect else ("LOWERED",))
            source("as_build.count", "infoCount")
            source("as_build.infos", "pInfos")
            if indirect:
                source("as_build.indirect_addresses", "pIndirectDeviceAddresses")
                source("as_build.indirect_strides", "pIndirectStrides")
                source("as_build.max_primitive_counts", "ppMaxPrimitiveCounts")
            else:
                source("as_build.ranges", "ppBuildRangeInfos")
        elif n in ("vkCmdCopyAccelerationStructureKHR", "vkCmdCopyAccelerationStructureToMemoryKHR",
                   "vkCmdCopyMemoryToAccelerationStructureKHR"):
            kind("AS_COPY", physical=stage + "COMPUTE_SHADER_BIT", flags=("LOWERED", "AS_COPY_CONTEXT"))
            variants = {"vkCmdCopyAccelerationStructureKHR": ("AS_CLONE_COMPACT", "copy"),
                        "vkCmdCopyAccelerationStructureToMemoryKHR": ("AS_SERIALIZE", "serialize"),
                        "vkCmdCopyMemoryToAccelerationStructureKHR": ("AS_DESERIALIZE", "deserialize")}
            variant, field = variants[n]
            assign("as_copy.kind", flag + variant)
            source("as_copy." + field, "pInfo")
        elif n in ("vkCmdTraceRaysKHR", "vkCmdTraceRaysIndirectKHR", "vkCmdTraceRaysIndirect2KHR"):
            indirect = n != "vkCmdTraceRaysKHR"
            logical = stage + "RAY_TRACING_SHADER_BIT_KHR"
            if indirect:
                logical += " | " + stage + "DRAW_INDIRECT_BIT"
            kind("TRACE", logical, stage + "COMPUTE_SHADER_BIT",
                 ("LOWERED", "INDIRECT") if indirect else ("LOWERED",))
            if "pRaygenShaderBindingTable" in names:
                for field, member in (("raygen", "pRaygenShaderBindingTable"), ("miss", "pMissShaderBindingTable"),
                                      ("hit", "pHitShaderBindingTable"), ("callable", "pCallableShaderBindingTable")):
                    source("trace." + field, member)
            if indirect:
                source("trace.indirect_address", "indirectDeviceAddress")
            else:
                for member in ("width", "height", "depth"):
                    source("trace." + member, member)
        elif n == "vkCmdExecuteCommands":
            kind("EXECUTE_SECONDARIES")
            source("children.count", "commandBufferCount")
            source("children.commands", "pCommandBuffers")
        elif n in ("vkCmdBeginRenderPass", "vkCmdBeginRenderPass2", "vkCmdNextSubpass", "vkCmdNextSubpass2",
                   "vkCmdEndRenderPass", "vkCmdEndRenderPass2", "vkCmdBeginRendering", "vkCmdEndRendering"):
            k = "RENDER_BEGIN" if n.startswith("vkCmdBegin") else "RENDER_NEXT" if n.startswith("vkCmdNext") else "RENDER_END"
            kind(k, flags=("GRAPHICS_CONTEXT",))
            assign("render.kind", flag + ("DYNAMIC_RENDERING" if n.endswith("Rendering") else "RENDER_PASS"))
            if "pRenderPassBegin" in names:
                source("render.render_pass", "pRenderPassBegin")
            if "pRenderingInfo" in names:
                source("render.rendering", "pRenderingInfo")
            if "contents" in names:
                source("render.contents", "contents")
            if "pSubpassBeginInfo" in names:
                required("pSubpassBeginInfo")
                source("render.subpass_begin", "pSubpassBeginInfo")
                assign("render.contents", "saved->pSubpassBeginInfo->contents")
            if "pSubpassEndInfo" in names:
                source("render.subpass_end", "pSubpassEndInfo")
        elif n in ("vkCmdBeginConditionalRenderingEXT", "vkCmdEndConditionalRenderingEXT"):
            kind("CONDITIONAL_BEGIN" if n.startswith("vkCmdBegin") else "CONDITIONAL_END")
            if "pConditionalRenderingBegin" in names:
                source("conditional.begin", "pConditionalRenderingBegin")
        elif n in ("vkCmdBeginTransformFeedbackEXT", "vkCmdEndTransformFeedbackEXT"):
            kind("TRANSFORM_FEEDBACK_BEGIN" if n.startswith("vkCmdBegin") else "TRANSFORM_FEEDBACK_END",
                 stage + "TRANSFORM_FEEDBACK_BIT_EXT", stage + "TRANSFORM_FEEDBACK_BIT_EXT")
            source("transform_feedback.first", "firstCounterBuffer")
            source("transform_feedback.count", "counterBufferCount")
            source("transform_feedback.counter_buffers", "pCounterBuffers")
            source("transform_feedback.counter_offsets", "pCounterBufferOffsets")
        elif n in ("vkCmdDispatch", "vkCmdDispatchBase", "vkCmdDispatchIndirect"):
            logical = stage + "COMPUTE_SHADER_BIT"
            if n == "vkCmdDispatchIndirect":
                logical += " | " + stage + "DRAW_INDIRECT_BIT"
            kind("ACTION", logical, logical, ("INDIRECT",) if n == "vkCmdDispatchIndirect" else ())
        elif n in ("vkCmdCopyBuffer", "vkCmdCopyBuffer2", "vkCmdCopyImage", "vkCmdCopyImage2",
                   "vkCmdCopyBufferToImage", "vkCmdCopyBufferToImage2", "vkCmdCopyImageToBuffer", "vkCmdCopyImageToBuffer2",
                   "vkCmdUpdateBuffer", "vkCmdFillBuffer", "vkCmdClearColorImage", "vkCmdClearDepthStencilImage",
                   "vkCmdBlitImage", "vkCmdBlitImage2", "vkCmdResolveImage", "vkCmdResolveImage2"):
            s = "CLEAR_BIT" if n in ("vkCmdUpdateBuffer", "vkCmdFillBuffer", "vkCmdClearColorImage", "vkCmdClearDepthStencilImage") else \
                "BLIT_BIT" if n.startswith("vkCmdBlit") else "RESOLVE_BIT" if n.startswith("vkCmdResolve") else "COPY_BIT"
            kind("ACTION", stage + s, stage + s)
        elif n in ("vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect",
                   "vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount", "vkCmdDrawIndirectByteCountEXT",
                   "vkCmdDrawMultiEXT", "vkCmdDrawMultiIndexedEXT", "vkCmdDrawMeshTasksNV", "vkCmdDrawMeshTasksIndirectNV",
                   "vkCmdDrawMeshTasksIndirectCountNV", "vkCmdDrawMeshTasksEXT", "vkCmdDrawMeshTasksIndirectEXT",
                   "vkCmdDrawMeshTasksIndirectCountEXT", "vkCmdClearAttachments"):
            indirect = "Indirect" in n
            flags = ("GRAPHICS_CONTEXT", "INDIRECT") if indirect else ("GRAPHICS_CONTEXT",)
            kind("ACTION", stage + "DRAW_INDIRECT_BIT" if indirect else None,
                 stage + "DRAW_INDIRECT_BIT" if indirect else None, flags)
        elif c.get("tasks", "").split(",") == ["state"]:
            # Registry task classification states that no action or sync work
            # is performed. This is not a prefix-based guess for new commands.
            kind("STATE")
            assign("state.kind", flag + "STATE_DYNAMIC")
            assign("state.bind_point", "VK_PIPELINE_BIND_POINT_MAX_ENUM")
            if c.get("renderpass") == "inside":
                lines.append("    metadata->flags |= VKRT_REPLAY_RENDER_LOCAL;")
            if c.get("videocoding") == "inside":
                lines.append("    metadata->flags |= VKRT_REPLAY_VIDEO_LOCAL;")
            if n in ("vkCmdSetRenderingAttachmentLocations", "vkCmdSetRenderingInputAttachmentIndices"):
                lines.append("    metadata->flags |= VKRT_REPLAY_RENDER_LOCAL;")
            if "pipelineBindPoint" in names:
                source("state.bind_point", "pipelineBindPoint")
            if n == "vkCmdBindPipeline":
                assign("state.kind", flag + "STATE_PIPELINE")
                source("state.pipeline", "pipeline")
            elif n == "vkCmdBindShadersEXT":
                assign("state.kind", flag + "STATE_SHADERS")
                lines.append("    if (saved->stageCount && !saved->pStages) return VK_ERROR_FEATURE_NOT_PRESENT;")
                source("state.shader_count", "stageCount")
                source("state.shader_stages", "pStages")
                source("state.shaders", "pShaders")
                lines.extend(["    for (uint32_t i = 0; i < saved->stageCount; ++i)",
                              "        metadata->data.state.affected_stages |= saved->pStages[i];"])
            elif n in ("vkCmdPushConstants", "vkCmdPushConstants2"):
                assign("state.kind", flag + "STATE_PUSH_CONSTANTS")
                if "stageFlags" in names:
                    source("state.affected_stages", "stageFlags")
            elif n in ("vkCmdBindIndexBuffer", "vkCmdBindIndexBuffer2", "vkCmdBindIndexBuffer3KHR", "vkCmdBindVertexBuffers",
                       "vkCmdBindVertexBuffers2", "vkCmdBindVertexBuffers3KHR", "vkCmdSetVertexInputEXT"):
                assign("state.kind", flag + "STATE_VERTEX_INDEX")
            elif n == "vkCmdSetDeviceMask":
                assign("state.kind", flag + "STATE_DEVICE_MASK")
                source("state.device_mask", "deviceMask")
            elif n == "vkCmdSetRayTracingPipelineStackSizeKHR":
                assign("state.kind", flag + "STATE_RT_STACK")
                source("state.stack_size", "pipelineStackSize")
            elif "Descriptor" in n:
                assign("state.kind", flag + "STATE_DESCRIPTORS")
            info_stages = {"vkCmdBindDescriptorSets2": "pBindDescriptorSetsInfo", "vkCmdPushConstants2": "pPushConstantsInfo",
                           "vkCmdPushDescriptorSet2": "pPushDescriptorSetInfo",
                           "vkCmdSetDescriptorBufferOffsets2EXT": "pSetDescriptorBufferOffsetsInfo",
                           "vkCmdBindDescriptorBufferEmbeddedSamplers2EXT": "pBindDescriptorBufferEmbeddedSamplersInfo"}
            if n in info_stages:
                member = info_stages[n]
                required(member)
                assign("state.affected_stages", "saved->" + member + "->stageFlags")
        else:
            return []
        return lines

    def command(self, n, c, params):
        ret = c.findtext("./proto/type")
        args = [name_of(p) for p in params]
        first = args[0]
        lines = ["struct replay_args_" + n + " {"]
        lines.extend("    " + self.saved_decl(p) for p in params[1:])
        if len(params) == 1:
            lines.append("    unsigned char unused;")
        lines.append("};")
        lines.append("static void replay_clone_args_" + n + "(struct replay_arena *arena, struct replay_args_" + n + " *dst, const struct replay_args_" + n + " *src) {")
        lines.append("    (void)arena;")
        lines.append("    *dst = *src;")
        capture = []
        reasons = []
        env = {a: "src->" + a for a in args[1:]}
        for p in params[1:]:
            try:
                self.clone_member(p, "dst->" + name_of(p), "src->" + name_of(p), env, capture, owner=c.findtext("./proto/name"))
            except (ValueError, SyntaxError) as exc:
                reasons.append(name_of(p) + ": " + str(exc))
        if reasons:
            lines.append("    replay_unsupported(arena);")
            self.command_reasons[n] = reasons
        else:
            lines.extend(capture)
        lines.append("}")
        lines.extend(["static VkResult replay_emit_" + n + "(const void *data, VkCommandBuffer target, vkrt_replay_resolve resolve, void *user, vkrt_replay_remap_child remap_child, const VkAllocationCallbacks *allocator, VkSystemAllocationScope scope) {",
                      "    (void)remap_child;",
                      "    const struct replay_args_" + n + " *saved = data;",
                      "    PFN_" + n + " proc = (PFN_" + n + ")resolve(user, \"" + n + "\");",
                      "    if (!proc) return VK_ERROR_FEATURE_NOT_PRESENT;",
                      "    struct replay_arena temporary = {.allocator = allocator, .scope = scope};",
                      "    struct vkrt_replay_context *context = vkrt_replay_lookup_context(target);",
                      "    if (context && context->emit_pipeline_layout) {",
                      "        temporary.emit_pipeline_layout = context->emit_pipeline_layout;",
                      "        temporary.command_name = \"" + n + "\";",
                      "        temporary.user = context->user;",
                      "        struct replay_args_" + n + " *copy = replay_alloc(&temporary, 1, sizeof(*copy));",
                      "        if (copy) replay_clone_args_" + n + "(&temporary, copy, saved);",
                      "        if (temporary.status != VK_SUCCESS) { VkResult error = temporary.status; replay_arena_finish(&temporary); return error; }",
                      "        saved = copy;",
                      "    }"])
        emit_args = ["target"] + ["saved->" + a for a in args[1:]]
        if n == "vkCmdExecuteCommands":
            lines.extend(["    VkCommandBuffer *children = NULL;",
                          "    if (remap_child && saved->commandBufferCount) {",
                          "        children = replay_alloc(&temporary, saved->commandBufferCount, sizeof(*children));",
                          "        if (!children) { replay_arena_finish(&temporary); return temporary.status; }",
                          "        for (uint32_t i = 0; i < saved->commandBufferCount; ++i) {",
                          "            VkResult result = remap_child(user, saved->pCommandBuffers[i], &children[i]);",
                          "            if (result != VK_SUCCESS) { replay_arena_finish(&temporary); return result; }",
                          "        }",
                          "        proc(target, saved->commandBufferCount, children);",
                          "        replay_arena_finish(&temporary);", "        return VK_SUCCESS;", "    }"])
        prefix = "    VkResult result = " if ret == "VkResult" else "    "
        lines.append(prefix + "proc(" + ", ".join(emit_args) + ");")
        lines.append("    replay_arena_finish(&temporary);")
        if ret != "VkResult":
            lines.append("    return VK_SUCCESS;")
        else:
            lines.append("    return result;")
        lines.append("}")
        metadata = self.metadata(c, params)
        if metadata:
            lines.extend(["static VkResult replay_metadata_" + n + "(const void *data, struct vkrt_replay_metadata *metadata) {",
                          "    const struct replay_args_" + n + " *saved = data;", "    (void)saved;"])
            lines.extend(metadata)
            lines.extend(["    return VK_SUCCESS;", "}"])
        capture_sig = ", ".join(["struct vkrt_replay_context *context"] + [self.shape(p)[2] for p in params[1:]])
        lines.extend(["static VKAPI_ATTR VkResult VKAPI_CALL replay_capture_" + n + "(" + capture_sig + ") {",
                      "    if (!context || !context->journal) return VK_ERROR_INITIALIZATION_FAILED;",
                      "    if (vkrt_replay_journal_status(context->journal) != VK_SUCCESS) return vkrt_replay_journal_status(context->journal);"])
        if n in self.TEMPLATE_COMMANDS:
            lines.append("    if (context->capture_descriptor_template) {")
            if n in ("vkCmdPushDescriptorSetWithTemplate", "vkCmdPushDescriptorSetWithTemplateKHR"):
                lines.extend(["        const VkPushDescriptorSetWithTemplateInfo info = {",
                              "            .sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO,",
                              "            .descriptorUpdateTemplate = descriptorUpdateTemplate, .layout = layout, .set = set, .pData = pData};"])
                template_info = "&info"
            else:
                template_info = args[1]
            lines.extend(["        VkResult result = context->capture_descriptor_template(context, " + template_info + ");",
                          "        if (result != VK_SUCCESS) {",
                          "            struct replay_arena failure = replay_capture_begin(context);",
                          "            if (failure.status == VK_SUCCESS) failure.status = result;",
                          "            replay_capture_end(context, \"" + n + "\", NULL, NULL, NULL, &failure);",
                          "        }", "        return vkrt_replay_journal_status(context->journal);", "    }"])
        lines.extend([
                      "    struct replay_arena storage = replay_capture_begin(context);",
                      "    struct replay_arena *arena = &storage;",
                      "    struct replay_args_" + n + " *saved = replay_alloc(arena, 1, sizeof(*saved));",
                      "    if (saved) {"])
        initializers = ["." + a + " = " + a for a in args[1:]] or [".unused = 0"]
        lines.append("        struct replay_args_" + n + " source = {" + ", ".join(initializers) + "};")
        lines.append("        replay_clone_args_" + n + "(arena, saved, &source);")
        describe = "replay_metadata_" + n if metadata else "NULL"
        lines.extend(["    }", "    replay_capture_end(context, \"" + n + "\", replay_emit_" + n + ", " + describe + ", saved, arena);",
                      "    return vkrt_replay_journal_status(context->journal);", "}"])
        sig = ", ".join(self.shape(p)[2] for p in params)
        capture_args = ["context"] + args[1:]
        lines.extend(["static VKAPI_ATTR " + ret + " VKAPI_CALL replay_record_" + n + "(" + sig + ") {",
                      "    struct vkrt_replay_context *context = vkrt_replay_lookup_context(" + first + ");",
                      "    if (context && context->journal) replay_capture_" + n + "(" + ", ".join(capture_args) + ");",
                      "    PFN_" + n + " proc = context && context->resolve ? (PFN_" + n + ")context->resolve(context->user, \"" + n + "\") : NULL;"])
        if ret == "void":
            lines.append("    if (proc) proc(" + ", ".join(args) + ");")
        elif ret == "VkResult":
            lines.append("    return proc ? proc(" + ", ".join(args) + ") : VK_ERROR_FEATURE_NOT_PRESENT;")
        else:
            raise ValueError("unsupported command return type " + ret)
        lines.append("}")
        return lines

    def generate(self):
        forwarding = []
        for n, c, params in self.header_missing_commands:
            try:
                ret, signature = self.forwarding_signature(c, params)
                forwarding.append((n, c, params, ret, signature))
            except ValueError as exc:
                self.abi_rejections[n] = str(exc)
        self.add_type("VkCommandBufferBeginInfo")
        for _, _, params in self.commands:
            for p in params[1:]:
                self.add_type(p.findtext("type"))
        lines = ["/* Generated by generate_command_replay.py; do not edit. */",
                 "static void *replay_clone_chain(struct replay_arena *arena, const void *source);"]
        lines.extend(self.abi_declarations.values())
        for n in sorted(self.reachable):
            lines.append("static inline void replay_clone_" + n + "(struct replay_arena *, " + n + " *, const " + n + " *);")
        for n in sorted(self.reachable):
            lines.extend(self.struct_clone(n))
        lines.extend(["static void *replay_clone_chain_value(struct replay_arena *arena, const void *source) {",
                      "    if (!source || arena->status != VK_SUCCESS) return NULL;",
                      "    const VkBaseInStructure *base = source;", "    switch (base->sType) {"])
        stypes = set()
        for n in sorted(self.reachable):
            stype = next((m.get("values") for m in self.members(n) if name_of(m) == "sType"), None)
            if stype and stype not in stypes:
                stypes.add(stype)
                lines.append("    case " + stype + ": {")
                if n in self.INACTIVE_DYNAMIC_INHERITANCE:
                    lines.extend(["        if (arena->in_inheritance && !arena->dynamic_rendering_inheritance)",
                                  "            return replay_clone_chain(arena, base->pNext);"])
                lines.extend(["        " + n + " *copy = replay_alloc(arena, 1, sizeof(*copy));",
                              "        if (copy) replay_clone_" + n + "(arena, copy, source);", "        return copy;", "    }"])
        lines.extend(["    default: replay_unsupported(arena); return NULL;", "    }", "}",
                      "static void *replay_clone_chain(struct replay_arena *arena, const void *source) {",
                      "    if (!source || arena->status != VK_SUCCESS) return NULL;",
                      "    for (const struct replay_chain_frame *p = arena->chain_path; p; p = p->previous)",
                      "        if (p->source == source) { replay_unsupported(arena); return NULL; }",
                      "    const struct replay_chain_frame frame = {source, arena->chain_path};",
                      "    arena->chain_path = &frame;",
                      "    void *copy = replay_clone_chain_value(arena, source);",
                      "    arena->chain_path = frame.previous;",
                      "    return copy;", "}"])
        for n, c, params in self.commands:
            lines.extend(self.command(n, c, params))
        for n, c, params, ret, signature in forwarding:
            lines.extend(self.forwarding_command(n, c, params, ret, signature))
        lines.extend(["PFN_vkVoidFunction vkrt_replay_proc(const char *name) {", "    if (!name) return NULL;"])
        for n, _, _ in self.commands:
            lines.append("    if (!strcmp(name, \"" + n + "\")) return (PFN_vkVoidFunction)replay_record_" + n + ";")
        for n, _, _, _, _ in forwarding:
            lines.append("    if (!strcmp(name, \"" + n + "\")) return (PFN_vkVoidFunction)replay_record_" + n + ";")
        lines.extend(["    return NULL;", "}", ""])
        lines.extend(["PFN_vkVoidFunction vkrt_replay_capture_proc(const char *name) {", "    if (!name) return NULL;"])
        for n, _, _ in self.commands:
            lines.append("    if (!strcmp(name, \"" + n + "\")) return (PFN_vkVoidFunction)replay_capture_" + n + ";")
        lines.extend(["    return NULL;", "}", ""])
        # Attribute conditional restrictions to each command as well as listing
        # schemas globally. A typed outer signature is not evidence that opaque
        # optional payloads (video standard structures, templates, etc.) work.
        edges = {}
        for n in self.walked:
            edges[n] = {self.canonical(m.findtext("type")) for m in self.members(n)} & self.walked
        for n, node in self.types.items():
            target = self.canonical(n)
            if target not in self.walked:
                continue
            for parent in node.get("structextends", "").split(","):
                parent = self.canonical(parent)
                if parent in edges:
                    edges[parent].add(target)
        command_manifest = []
        for n, command, params in self.commands:
            seen, pending = set(), [self.canonical(p.findtext("type")) for p in params[1:]]
            while pending:
                t = pending.pop()
                if t in seen:
                    continue
                seen.add(t)
                pending.extend(edges.get(t, ()))
            restrictions = {t: self.struct_reasons[t] for t in sorted(seen) if t in self.struct_reasons}
            capture_kind = "owned_template_normalization_or_unsupported" if n in self.TEMPLATE_COMMANDS else (
                "unsupported" if n in self.command_reasons else "typed_with_runtime_schema_checks")
            command_manifest.append({"name": n,
                                     "capture": capture_kind,
                                     "metadata": "reviewed_or_registry_state" if self.metadata(command, params) else "incomplete",
                                     "reasons": self.command_reasons.get(n, []),
                                     "conditional_schema_restrictions": restrictions})
        for n, _, _, _, _ in forwarding:
            command_manifest.append({"name": n, "capture": "unsupported",
                                     "metadata": "incomplete",
                                     "reasons": ["ABI-only forwarding: command is absent from the selected core header; actual invocation fails capture without reading opaque inputs"],
                                     "conditional_schema_restrictions": {}})
        command_manifest.sort(key=lambda x: x["name"])
        wrapped_names = {c[0] for c in self.commands} | {c[0] for c in forwarding}
        manifest = {"registry_sha256": hashlib.sha256(self.registry.read_bytes()).hexdigest(),
                    "header_sha256": hashlib.sha256(self.header.read_bytes()).hexdigest(),
                    "policy": "Typed command inputs are owned; unknown pNext, opaque schemas, and unsupported union cases fail capture. Native handles obey Vulkan object lifetimes. Pipeline layouts are normalized by the caller callback. Device-address unions retain address bits, opaque checkpoint markers retain identity.",
                    "command_count": len(wrapped_names), "typed_struct_count": len(self.reachable),
                    "capture_only_command_count": len(self.commands),
                    "template_normalization": "An integration-provided immutable template schema normalizes template payloads into ordinary owning push entries; missing metadata fails capture without borrowing pData.",
                    "metadata_command_count": sum(c["metadata"] != "incomplete" for c in command_manifest),
                    "metadata_policy": "Reviewed action/synchronization facts and registry state-only tasks; raw scopes and context flags are not complete dependency/replay certificates. Unknown action semantics fail metadata queries without poisoning ordinary capture/replay.",
                    "commands": command_manifest,
                    "conditional_struct_restrictions": self.struct_reasons,
                    "manual_pointer_rules": self.MANUAL_POINTER_RULES,
                    "inactive_dynamic_inheritance_skips": sorted(self.INACTIVE_DYNAMIC_INHERITANCE),
                    "selected_union_schemas": sorted(self.selected_unions | {"VkAccelerationStructureGeometryDataKHR"}),
                    "unsupported_union_schemas": sorted(n for n in self.walked if self.category(n) == "union" and not self.pod_union(n) and n != "VkAccelerationStructureGeometryDataKHR" and n not in self.selected_unions),
                    "header_unavailable_commands": [n for n, _, _ in self.header_missing_commands],
                    "unsupported_forwarding_commands": [n for n, _, _, _, _ in forwarding],
                    "abi_rejections": self.abi_rejections,
                    "unavailable_commands": sorted(n for n in self.all_commands if n and n.startswith("vkCmd") and n not in wrapped_names)}
        return "\n".join(lines), manifest

    def capture_header(self):
        lines = ["/* Generated by generate_command_replay.py; do not edit. */",
                 "#ifndef VKRT_REPLAY_CAPTURE_GENERATED_H", "#define VKRT_REPLAY_CAPTURE_GENERATED_H",
                 "#include <vulkan/vulkan.h>", "struct vkrt_replay_context;"]
        for n, _, params in self.commands:
            signature = ", ".join(["struct vkrt_replay_context *context"] + [self.shape(p)[2] for p in params[1:]])
            lines.append("typedef VkResult (VKAPI_PTR *PFN_vkrt_capture_" + n[2:] + ")(" + signature + ");")
        lines.extend(["#endif", ""])
        return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--coverage", required=True, type=Path)
    parser.add_argument("--capture-header", type=Path)
    args = parser.parse_args()
    generator = Generator(args.registry, args.header)
    output, manifest = generator.generate()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.coverage.parent.mkdir(parents=True, exist_ok=True)
    args.coverage.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if manifest["abi_rejections"]:
        parser.error("Cannot generate ABI-safe command admission blockers: " + json.dumps(manifest["abi_rejections"]))
    args.output.write_text(output, encoding="utf-8")
    capture_header = args.capture_header or args.output.with_name("vkrt_replay_capture_generated.h")
    capture_header.parent.mkdir(parents=True, exist_ok=True)
    capture_header.write_text(generator.capture_header(), encoding="utf-8")
    unsupported = sum(c["capture"] == "unsupported" for c in manifest["commands"])
    print(f"command replay: {manifest['command_count']} typed wrappers, {unsupported} explicit unsupported commands, {manifest['typed_struct_count']} struct schemas")


if __name__ == "__main__":
    main()
