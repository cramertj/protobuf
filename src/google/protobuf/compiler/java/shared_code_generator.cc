// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// Author: xiaofeng@google.com (Feng Xiao)

#include "google/protobuf/compiler/java/shared_code_generator.h"

#include <memory>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "google/protobuf/compiler/code_generator.h"
#include "google/protobuf/compiler/java/helpers.h"
#include "google/protobuf/compiler/java/name_resolver.h"
#include "google/protobuf/compiler/java/names.h"
#include "google/protobuf/compiler/retention.h"
#include "google/protobuf/compiler/versions.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/io/printer.h"
#include "google/protobuf/io/zero_copy_stream.h"

namespace google {
namespace protobuf {
namespace compiler {
namespace java {

SharedCodeGenerator::SharedCodeGenerator(const FileDescriptor* file,
                                         const Options& options)
    : name_resolver_(new ClassNameResolver(options)),
      file_(file),
      options_(options) {}

SharedCodeGenerator::~SharedCodeGenerator() {}

void SharedCodeGenerator::Generate(
    GeneratorContext* context, std::vector<std::string>* file_list,
    std::vector<std::string>* annotation_file_list) {
  std::string java_package = FileJavaPackage(file_, true, options_);
  std::string package_dir = JavaPackageToDir(java_package);

  if (HasDescriptorMethods(file_, options_.enforce_lite)) {
    // Generate descriptors.
    std::string classname = name_resolver_->GetDescriptorClassName(file_);
    std::string filename = absl::StrCat(package_dir, classname, ".java");
    file_list->push_back(filename);
    std::unique_ptr<io::ZeroCopyOutputStream> output(context->Open(filename));
    GeneratedCodeInfo annotations;
    io::AnnotationProtoCollector<GeneratedCodeInfo> annotation_collector(
        &annotations);
    std::unique_ptr<io::Printer> printer(new io::Printer(
        output.get(), '$',
        options_.annotate_code ? &annotation_collector : nullptr));
    std::string info_relative_path = absl::StrCat(classname, ".java.pb.meta");
    std::string info_full_path = absl::StrCat(filename, ".pb.meta");
    printer->Print(
        "// Generated by the protocol buffer compiler.  DO NOT EDIT!\n"
        "// NO CHECKED-IN PROTOBUF "
        // Intentional line breaker
        "GENCODE\n"
        "// source: $filename$\n",
        "filename", file_->name());
    if (options_.opensource_runtime) {
      printer->Print("// Protobuf Java Version: $protobuf_java_version$\n",
                     "protobuf_java_version", PROTOBUF_JAVA_VERSION_STRING);
    }
    printer->Print("\n");
    if (!java_package.empty()) {
      printer->Print(
          "package $package$;\n"
          "\n",
          "package", java_package);
    }
    PrintGeneratedAnnotation(printer.get(), '$',
                             options_.annotate_code ? info_relative_path : "",
                             options_);


    printer->Print(

        "public final class $classname$ {\n"
        "  /* This variable is to be called by generated code only. It "
        "returns\n"
        "  * an incomplete descriptor for internal use only. */\n"
        "  public static com.google.protobuf.Descriptors.FileDescriptor\n"
        "      descriptor;\n",
        "classname", classname);
    printer->Annotate("classname", file_->name());


    printer->Print("  static {\n");
    printer->Indent();
    printer->Indent();
    GenerateDescriptors(printer.get());
    PrintGencodeVersionValidator(printer.get(), options_.opensource_runtime,
                                 classname);
    printer->Outdent();
    printer->Outdent();
    printer->Print(
        "  }\n"
        "}\n");

    if (options_.annotate_code) {
      std::unique_ptr<io::ZeroCopyOutputStream> info_output(
          context->Open(info_full_path));
      annotations.SerializeToZeroCopyStream(info_output.get());
      annotation_file_list->push_back(info_full_path);
    }

    printer.reset();
    output.reset();
  }
}

void SharedCodeGenerator::GenerateDescriptors(io::Printer* printer) {
  // Embed the descriptor.  We simply serialize the entire FileDescriptorProto
  // and embed it as a string literal, which is parsed and built into real
  // descriptors at initialization time.  We unfortunately have to put it in
  // a string literal, not a byte array, because apparently using a literal
  // byte array causes the Java compiler to generate *instructions* to
  // initialize each and every byte of the array, e.g. as if you typed:
  //   b[0] = 123; b[1] = 456; b[2] = 789;
  // This makes huge bytecode files and can easily hit the compiler's internal
  // code size limits (error "code to large").  String literals are apparently
  // embedded raw, which is what we want.
  FileDescriptorProto file_proto = StripSourceRetentionOptions(*file_);
  // Skip serialized file descriptor proto, which contain non-functional
  // deviation between editions and legacy syntax (e.g. syntax, features)
  if (options_.strip_nonfunctional_codegen) {
    file_proto.Clear();
  }

  std::string file_data;
  file_proto.SerializeToString(&file_data);

  printer->Print("java.lang.String[] descriptorData = {\n");
  printer->Indent();

  // Limit the number of bytes per line.
  static const int kBytesPerLine = 40;
  // Limit the number of lines per string part.
  static const int kLinesPerPart = 400;
  // Every block of bytes, start a new string literal, in order to avoid the
  // 64k length limit. Note that this value needs to be <64k.
  static const int kBytesPerPart = kBytesPerLine * kLinesPerPart;
  for (int i = 0; i < file_data.size(); i += kBytesPerLine) {
    if (i > 0) {
      if (i % kBytesPerPart == 0) {
        printer->Print(",\n");
      } else {
        printer->Print(" +\n");
      }
    }
    printer->Print("\"$data$\"", "data",
                   absl::CEscape(file_data.substr(i, kBytesPerLine)));
  }

  printer->Outdent();
  printer->Print("\n};\n");

  // -----------------------------------------------------------------
  // Find out all dependencies.
  std::vector<std::pair<std::string, std::string> > dependencies;
  for (int i = 0; i < file_->dependency_count(); i++) {
    std::string filename = file_->dependency(i)->name();
    std::string package = FileJavaPackage(file_->dependency(i), true, options_);
    std::string classname =
        name_resolver_->GetDescriptorClassName(file_->dependency(i));
    std::string full_name;
    if (package.empty()) {
      full_name = classname;
    } else {
      full_name = absl::StrCat(package, ".", classname);
    }
    dependencies.push_back(std::make_pair(filename, full_name));
  }

  // -----------------------------------------------------------------
  // Invoke internalBuildGeneratedFileFrom() to build the file.
  printer->Print(
      "descriptor = com.google.protobuf.Descriptors.FileDescriptor\n"
      "  .internalBuildGeneratedFileFrom(descriptorData,\n");
  if (options_.opensource_runtime) {
    printer->Print(
        "    new com.google.protobuf.Descriptors.FileDescriptor[] {\n");

    for (int i = 0; i < dependencies.size(); i++) {
      absl::string_view dependency = dependencies[i].second;
      printer->Print("      $dependency$.getDescriptor(),\n", "dependency",
                     dependency);
    }
  }

  printer->Print("    });\n");
}

}  // namespace java
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
