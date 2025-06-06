/*****************************************************************************************
 *                                                                                       *
 * GHOUL                                                                                 *
 * General Helpful Open Utility Library                                                  *
 *                                                                                       *
 * Copyright (c) 2012-2025                                                               *
 *                                                                                       *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this  *
 * software and associated documentation files (the "Software"), to deal in the Software *
 * without restriction, including without limitation the rights to use, copy, modify,    *
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to    *
 * permit persons to whom the Software is furnished to do so, subject to the following   *
 * conditions:                                                                           *
 *                                                                                       *
 * The above copyright notice and this permission notice shall be included in all copies *
 * or substantial portions of the Software.                                              *
 *                                                                                       *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,   *
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A         *
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT    *
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF  *
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  *
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                         *
 ****************************************************************************************/

#include <ghoul/opengl/shaderpreprocessor.h>

#include <ghoul/filesystem/filesystem.h>
#include <ghoul/format.h>
#include <ghoul/glm.h>
#include <ghoul/logging/log.h>
#include <ghoul/misc/dictionary.h>
#include <ghoul/misc/exception.h>
#include <ghoul/misc/stringhelper.h>
#include <ghoul/opengl/ghoul_gl.h>
#include <ghoul/systemcapabilities/openglcapabilitiescomponent.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>

namespace {
    bool isString(std::string_view str) {
        return str.length() > 1 && str.front() == '"' && str.back() == '"';
    }

    std::string trim(const std::string& str, std::string* before = nullptr,
                     std::string* after = nullptr)
    {
        static const std::string ws = " \n\r\t";
        size_t startPos = str.find_first_not_of(ws);
        if (startPos == std::string::npos) {
            startPos = 0;
        }
        size_t endPos = str.find_last_not_of(ws);
        if (endPos == std::string::npos) {
            endPos = str.length();
        }
        else {
            endPos += 1;
        }

        const size_t length = endPos - startPos;
        if (before) {
            *before = str.substr(0, startPos);
        }
        if (after) {
            *after = str.substr(endPos);
        }
        return str.substr(startPos, length);
    }

    std::string glslVersionString() {
        int versionMajor = 0;
        int versionMinor = 0;
        int profileMask = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
        glGetIntegerv(GL_MINOR_VERSION, &versionMinor);
        glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);

        const ContextProfileMask cpm = ContextProfileMask(profileMask);
        const bool isCore = cpm == ContextProfileMask::GL_CONTEXT_CORE_PROFILE_BIT;
        const bool isCompatibility =
            cpm == ContextProfileMask::GL_CONTEXT_COMPATIBILITY_PROFILE_BIT;

        ghoul_assert(
            isCore || isCompatibility,
            "OpenGL context is neither core nor compatibility"
        );

        std::string_view type =
            isCore ? " core" : (isCompatibility ? " compatibility" : "");
        return std::format("#version {}{}0 {}", versionMajor, versionMinor, type);
    }

    bool hasKeyRecursive(const ghoul::Dictionary& dictionary, std::string_view key) {
        const size_t dotPos = key.find('.');
        if (dotPos != std::string_view::npos) {
            const std::string_view before = key.substr(0, dotPos);
            const std::string_view after = key.substr(dotPos + 1);

            if (dictionary.hasKey(before)) {
                const ghoul::Dictionary d = dictionary.value<ghoul::Dictionary>(before);
                return d.hasKey(after);
            }
            else {
                return false;
            }
        }
        else {
            return dictionary.hasKey(key);
        }
    }

    template <typename T>
    bool hasValueRecursive(const ghoul::Dictionary& dictionary, std::string_view key) {
        const size_t dotPos = key.find('.');
        if (dotPos != std::string_view::npos) {
            const std::string_view before = key.substr(0, dotPos);
            const std::string_view after = key.substr(dotPos + 1);

            if (dictionary.hasValue<ghoul::Dictionary>(before)) {
                const ghoul::Dictionary d = dictionary.value<ghoul::Dictionary>(before);
                return d.hasValue<T>(after);
            }
            else {
                return false;
            }
        }
        else {
            return dictionary.hasValue<T>(key);
        }
    }

    template <typename T>
    T valueRecursive(const ghoul::Dictionary& dictionary, std::string_view key) {
        const size_t dotPos = key.find('.');
        if (dotPos != std::string_view::npos) {
            const std::string_view before = key.substr(0, dotPos);
            const std::string_view after = key.substr(dotPos + 1);

            const ghoul::Dictionary d = dictionary.value<ghoul::Dictionary>(before);
            return d.value<T>(after);
        }
        else {
            return dictionary.value<T>(key);
        }
    }
} // namespace

namespace ghoul::opengl {

std::vector<std::filesystem::path> ShaderPreprocessor::_includePaths =
    std::vector<std::filesystem::path>();

ShaderPreprocessor::ShaderPreprocessorError::ShaderPreprocessorError(std::string msg)
    : RuntimeError(std::move(msg), "ShaderPreprocessor")
{}

ShaderPreprocessor::SubstitutionError::SubstitutionError(std::string msg)
    : ShaderPreprocessorError(std::move(msg))
{}

ShaderPreprocessor::ParserError::ParserError(std::string msg)
    : ShaderPreprocessorError(std::move(msg))
{}

ShaderPreprocessor::ShaderPreprocessor(std::string shaderPath, Dictionary dictionary)
    : _shaderPath(std::move(shaderPath))
    , _dictionary(std::move(dictionary))
{}

ShaderPreprocessor::IncludeError::IncludeError(std::filesystem::path f)
    : ShaderPreprocessorError(
        std::format("Could not resolve file path for include file '{}'", f.string())
    )
    , file(std::move(f))
{}

ShaderPreprocessor::Input::Input(std::ifstream& str, ghoul::filesystem::File& f,
                                 std::string indent)
    : stream(str)
    , file(f)
    , indentation(std::move(indent))
{}

ShaderPreprocessor::Env::Env(std::stringstream& out, std::string l, std::string indent)
    : output(out)
    , line(std::move(l))
    , indentation(std::move(indent))
{}

void ShaderPreprocessor::setDictionary(Dictionary dictionary) {
    _dictionary = std::move(dictionary);
    if (_onChangeCallback) {
        _onChangeCallback();
    }
}

const Dictionary& ShaderPreprocessor::dictionary() const {
    return _dictionary;
}

void ShaderPreprocessor::setFilename(const std::filesystem::path& shaderPath) {
    if (_shaderPath != shaderPath) {
        _shaderPath = shaderPath;
        if (_onChangeCallback) {
            _onChangeCallback();
        }
    }
}

const std::filesystem::path& ShaderPreprocessor::filename() const {
    return _shaderPath;
}

void ShaderPreprocessor::process(std::string& output) {
    std::stringstream stream;
    ShaderPreprocessor::Env env{stream};

    includeFile(absPath(_shaderPath), TrackChanges::Yes, env);

    if (!env.forStatements.empty()) {
        throw ParserError(
            "Unexpected end of file in the middle of expanding #for statement. " +
            debugString(env)
        );
    }

    if (!env.scopes.empty()) {
        throw ParserError("Unexpected end of file. " + debugString(env));
    }

    output = stream.str();
}

void ShaderPreprocessor::setCallback(ShaderChangedCallback changeCallback) {
    _onChangeCallback = std::move(changeCallback);
    for (std::pair<const std::filesystem::path, FileStruct>& files : _includedFiles) {
        if (files.second.isTracked) {
            files.second.file.setCallback([this]() { _onChangeCallback(); });
        }
    }
}

std::string ShaderPreprocessor::getFileIdentifiersString() const {
    std::stringstream identifiers;
    for (const std::pair<const std::filesystem::path, FileStruct>& f : _includedFiles) {
        identifiers << f.second.fileIdentifier << ": " << f.first << '\n';
    }
    return identifiers.str();
}

void ShaderPreprocessor::addIncludePath(const std::filesystem::path& folderPath) {
    ghoul_assert(!folderPath.empty(), "Folder path must not be empty");
    ghoul_assert(
        std::filesystem::is_directory(folderPath),
        "Folder path must be an existing directory"
    );

    const auto it = std::find(_includePaths.begin(), _includePaths.end(), folderPath);
    if (it == _includePaths.cend()) {
        _includePaths.push_back(folderPath);
    }
}

void ShaderPreprocessor::includeFile(const std::filesystem::path& path,
                                     TrackChanges trackChanges,
                                     ShaderPreprocessor::Env& environment)
{
    ghoul_assert(!path.empty(), "Path must not be empty");
    ghoul_assert(std::filesystem::is_regular_file(path), "Path must be an existing file");

    if (_includedFiles.find(path) == _includedFiles.end()) {
        const auto it = _includedFiles.emplace(
            path,
            FileStruct {
                filesystem::File(path),
                _includedFiles.size(),
                trackChanges
            }
        ).first;
        if (trackChanges) {
            it->second.file.setCallback([this]() { _onChangeCallback(); });
        }
    }

    std::ifstream stream = std::ifstream(path, std::ifstream::binary);
    if (!stream.good()) {
        throw ghoul::RuntimeError(std::format("Error loading include file '{}'", path.string()));
    }
    ghoul_assert(stream.good() , "Input stream is not good");

    ghoul::filesystem::File file = ghoul::filesystem::File(path);

    const std::string prevIndent =
        !environment.inputs.empty() ? environment.inputs.back().indentation : "";

    environment.inputs.emplace_back(stream, file, prevIndent + environment.indentation);
    if (environment.inputs.size() > 1) {
        addLineNumber(environment);
    }

    while (parseLine(environment)) {
        if (!environment.success) {
            throw ParserError(std::format(
                "Could not parse line. '{}': {}",
                path.string(), environment.inputs.back().lineNumber
            ));
        }
    }

    if (!environment.forStatements.empty()) {
        const ShaderPreprocessor::ForStatement& forStatement =
            environment.forStatements.back();
        if (forStatement.inputIndex + 1 >= environment.inputs.size()) {
            const int inputIndex = forStatement.inputIndex;
            const ShaderPreprocessor::Input& forInput = environment.inputs[inputIndex];
            const std::filesystem::path p = forInput.file.path();
            int lineNumber = forStatement.lineNumber;

            throw ParserError(std::format(
                "Unexpected end of file. Still processing #for loop from '{}': {}. {}",
                p.string(), lineNumber, debugString(environment)
            ));
        }
    }

    environment.inputs.pop_back();

    if (!environment.inputs.empty()) {
        addLineNumber(environment);
    }
}

void ShaderPreprocessor::addLineNumber(ShaderPreprocessor::Env& env) {
    const std::filesystem::path filename = env.inputs.back().file.path();
    ghoul_assert(
        _includedFiles.find(filename) != _includedFiles.end(),
        "File not in included files"
    );
    const size_t fileIdentifier = _includedFiles.at(filename).fileIdentifier;

    std::string includeSeparator;
#ifndef __APPLE__
    // Sofar, only Nvidia on Windows supports empty statements in the middle of the shader
    using Vendor = ghoul::systemcapabilities::OpenGLCapabilitiesComponent::Vendor;
    if (OpenGLCap.gpuVendor() == Vendor::Nvidia) {
        includeSeparator = "; // preprocessor add semicolon to isolate error messages";
    }
#endif // __APPLE__

    env.output << std::format(
        "{}\n#line {} {} // {}\n",
        includeSeparator, env.inputs.back().lineNumber, fileIdentifier, filename.string()
    );
}

bool ShaderPreprocessor::isInsideEmptyForStatement(ShaderPreprocessor::Env& env) {
    return !env.forStatements.empty() && (env.forStatements.back().keyIndex == -1);
}

bool ShaderPreprocessor::parseLine(ShaderPreprocessor::Env& env) {
    Input& input = env.inputs.back();
    if (!ghoul::getline(input.stream, env.line)) {
        return false;
    }
    input.lineNumber++;

    // Trim away any whitespaces in the start and end of the line.
    env.line = trim(env.line, &env.indentation);

    bool isSpecialLine = parseEndFor(env); // #endfor

    if (isInsideEmptyForStatement(env)) {
        return true;
    }

    // Replace all #{<name>} strings with data from <name> in dictionary.
    if (!substituteLine(env)) {
        return false;
    }

    if (!isSpecialLine) {
        isSpecialLine |=
            parseVersion(env) ||    // #version __CONTEXT__
            parseOs(env) ||         // #define __OS__
            parseInclude(env) ||    // #include
            parseFor(env);          // #for <key>, <value> in <dictionary>
    }

    if (!isSpecialLine) {
      // Write GLSL code to output.
        env.output << input.indentation << env.indentation << env.line << '\n';
    }
    return true;
    // Insert all extensions to the preprocessor here.
}

std::string ShaderPreprocessor::debugString(const ShaderPreprocessor::Env& env) {
    if (!env.inputs.empty()) {
        const ShaderPreprocessor::Input& input = env.inputs.back();
        return std::format("{}: {}", input.file.path().string(), input.lineNumber);
    }
    else {
        return "";
    }
}

bool ShaderPreprocessor::substituteLine(ShaderPreprocessor::Env& env) {
    std::string& line = env.line;
    size_t beginOffset = 0;

    while ((beginOffset = line.rfind("#{")) != std::string::npos) {
        const size_t endOffset = line.substr(beginOffset).find('}');
        if (endOffset == std::string::npos) {
            throw ParserError("Could not parse line. " + debugString(env));
        }

        const std::string in = line.substr(beginOffset + 2, endOffset - 2);
        const std::string out = substitute(in, env);

        const std::string first = line.substr(0, beginOffset);
        const std::string last = line.substr(
            beginOffset + endOffset + 1,
            line.length() - 1 - (beginOffset + endOffset)
        );

        line = std::format("{}{}{}", first, out, last);
    }
    return true;
}

bool ShaderPreprocessor::resolveAlias(const std::string& in, std::string& out,
                                      ShaderPreprocessor::Env& env)
{
    std::string beforeDot;
    std::string afterDot;
    if (const size_t firstDotPos = in.find('.');  firstDotPos != std::string::npos) {
        beforeDot = in.substr(0, firstDotPos);
        afterDot = in.substr(firstDotPos);
    }
    else {
        beforeDot = in;
        afterDot = "";
    }

    // Resolve only part before dot
    if (env.aliases.find(beforeDot) != env.aliases.end()) {
        if (!env.aliases[beforeDot].empty()) {
            beforeDot = env.aliases[beforeDot].back();
        }
    }

    out = beforeDot + afterDot;
    return ((afterDot.empty() && isString(beforeDot)) ||
           hasKeyRecursive(_dictionary, out));
}

std::string ShaderPreprocessor::substitute(const std::string& in,
                                           ShaderPreprocessor::Env& env)
{
    std::string resolved;
    if (!resolveAlias(in, resolved, env)) {
        throw SubstitutionError(std::format(
            "Could not resolve variable '{}'. {}", in, debugString(env)
        ));
    }

    if (isString(resolved)) {
        return resolved.substr(1, resolved.length() - 2);
    }
    else if (hasValueRecursive<bool>(_dictionary, resolved)) {
        return std::to_string(valueRecursive<bool>(_dictionary, resolved));
    }
    else if (hasValueRecursive<std::string>(_dictionary, resolved)) {
        return valueRecursive<std::string>(_dictionary, resolved);
    }
    else if (hasValueRecursive<int>(_dictionary, resolved)) {
        return std::to_string(valueRecursive<int>(_dictionary, resolved));
    }
    else if (hasValueRecursive<double>(_dictionary, resolved)) {
        return std::to_string(valueRecursive<double>(_dictionary, resolved));
    }
    else if (hasValueRecursive<glm::ivec2>(_dictionary, resolved)) {
        glm::ivec2 vec = valueRecursive<glm::ivec2>(_dictionary, resolved);
        return std::format("ivec2({},{})", vec.x, vec.y);
    }
    else if (hasValueRecursive<glm::ivec3>(_dictionary, resolved)) {
        glm::ivec3 vec = valueRecursive<glm::ivec3>(_dictionary, resolved);
        return std::format("ivec3({},{},{})", vec.x, vec.y, vec.z);
    }
    else if (hasValueRecursive<glm::dvec2>(_dictionary, resolved)) {
        glm::dvec2 vec = valueRecursive<glm::dvec2>(_dictionary, resolved);
        return std::format("dvec2({},{})", vec.x, vec.y);
    }
    else if (hasValueRecursive<glm::dvec3>(_dictionary, resolved)) {
        glm::dvec3 vec = valueRecursive<glm::dvec3>(_dictionary, resolved);
        return std::format("dvec3({},{},{})", vec.x, vec.y, vec.z);
    }
    else {
        throw SubstitutionError(std::format(
            "'{}' was resolved to '{}' which is a type that is not supported. {}",
            in, resolved, debugString(env)
        ));
    }
}

void ShaderPreprocessor::pushScope(const std::map<std::string, std::string>& map,
                                   ShaderPreprocessor::Env& env)
{
    Env::Scope scope;
    for (const std::pair<const std::string, std::string>& pair : map) {
        const std::string& key = pair.first;
        const std::string& value = pair.second;
        scope.insert(key);
        if (env.aliases.find(key) == env.aliases.end()) {
           env.aliases[key] = std::vector<std::string>();
        }
        env.aliases[key].push_back(value);
    }
    env.scopes.push_back(scope);
}

void ShaderPreprocessor::popScope(ShaderPreprocessor::Env& env) {
    ghoul_assert(!env.scopes.empty(), "Environment must have open scope");
    for (const std::string& key : env.scopes.back()) {
        (void)key;
        ghoul_assert(env.aliases.find(key) != env.aliases.end(), "Key not found");
        ghoul_assert(!env.aliases.at(key).empty(), "No aliases for key");
    }

    const Env::Scope& scope = env.scopes.back();
    for (const std::string& key : scope) {
        env.aliases[key].pop_back();
        if (env.aliases[key].empty()) {
            env.aliases.erase(key);
        }
    }
    env.scopes.pop_back();
}

bool ShaderPreprocessor::parseInclude(ShaderPreprocessor::Env& env) {
    static const std::string includeString = "#include";
    static const std::string ws = " \n\r\t";
    static const std::string noTrackString = ":notrack";

    std::string& line = env.line;

    const bool tracksInclude = (line.find(noTrackString) == std::string::npos);

    if (line.substr(0, includeString.length()) != includeString) {
        return false;
    }

    const size_t p1 = line.find_first_not_of(ws, includeString.length());
    if (p1 == std::string::npos) {
        throw ParserError("Expected file path after #include. " + debugString(env));
    }

    if ((line[p1] != '\"') && (line[p1] != '<')) {
        throw ParserError("Expected \" or <. " + debugString(env));
    }

    if (line[p1] == '\"') {
        const size_t p2 = line.find_first_of('\"', p1 + 1);
        if (p2 == std::string::npos) {
            throw ParserError("Expected \"" + debugString(env));
        }

        const size_t includeLength = p2 - p1 - 1;
        const std::filesystem::path includeFilename = line.substr(p1 + 1, includeLength);
        std::filesystem::path includeFilepath =
            env.inputs.back().file.path().parent_path() / includeFilename;

        bool includeFileWasFound = std::filesystem::is_regular_file(includeFilepath);

        // Resolve the include paths if this default includeFilename does not exist
        if (!includeFileWasFound) {
            for (const std::filesystem::path& path : _includePaths) {
                includeFilepath = path / includeFilename;
                if (std::filesystem::is_regular_file(includeFilepath)) {
                    includeFileWasFound = true;
                    break;
                }
            }
        }

        if (!includeFileWasFound) {
            // Our last chance is that the include file is an absolute path
            const bool found = std::filesystem::is_regular_file(includeFilename);
            if (found) {
                includeFilepath = includeFilename;
                includeFileWasFound = true;
            }
        }

        if (!includeFileWasFound) {
            throw IncludeError(includeFilename);
        }

        includeFile(absPath(includeFilepath), TrackChanges(tracksInclude), env);
    }
    else if (line.at(p1) == '<') {
        const size_t p2 = line.find_first_of('>', p1 + 1);
        if (p2 == std::string::npos) {
            throw ParserError("Expected >. " + debugString(env));
        }

        const size_t includeLen = p2 - p1 - 1;
        const std::filesystem::path include = absPath(line.substr(p1 + 1, includeLen));
        includeFile(include, TrackChanges(tracksInclude), env);
    }
    return true;
}

bool ShaderPreprocessor::parseVersion(const ShaderPreprocessor::Env& env) {
    static const std::string versionString = "#version __CONTEXT__";
    if (env.line.substr(0, versionString.length()) == versionString) {
        env.output << glslVersionString() << '\n';
        return true;
    }
    return false;
}

bool ShaderPreprocessor::parseOs(ShaderPreprocessor::Env& env) {
    static const std::string osString = "#define __OS__";
    const std::string& line = env.line;
    if (line.length() >= osString.length() &&
        line.substr(0, osString.length()) == osString)
    {
#ifdef WIN32
        constexpr std::string_view os = "WIN32";
#endif
#ifdef __APPLE__
        constexpr std::string_view os = "APPLE";
#endif
#ifdef __linux__
        constexpr std::string_view os = "linux";
#endif
        env.output << std::format(
            "#ifndef __OS__\n"
            "#define __OS__ {}\n"
            "#define {}\n"
            "#endif\n",
            os, os
        );
        addLineNumber(env);
        return true;
    }
    return false;
}

bool ShaderPreprocessor::tokenizeFor(const std::string& line, std::string& keyName,
                                     std::string& valueName, std::string& dictionaryName,
                                     ShaderPreprocessor::Env& env)
{
    static const std::string forString = "#for";
    static const std::string inString = "in";
    static const std::string ws = " \n\r\t";
    static const std::string comma = ",";

    // parse this:
    // #for <key>, <value> in <dictionary>

    const size_t length = line.length();
    if (length < forString.length() + inString.length() ||
        line.substr(0, forString.length()) != forString)
    {
        return false;
    }

    const size_t firstWsPos = forString.length();
    const size_t keyOffset = line.substr(firstWsPos).find_first_not_of(ws);
    const size_t keyPos = firstWsPos + keyOffset;

    const size_t commaOffset = line.substr(keyPos).find_first_of(comma);

    size_t commaPos = 0;
    if (commaOffset != std::string::npos) { // Found a comma
        commaPos = keyPos + commaOffset;
        keyName = trim(line.substr(keyPos, commaOffset));
    }
    else {
        commaPos = keyPos - 1;
        keyName = "";
    }

    const size_t valueOffset = line.substr(commaPos + 1).find_first_not_of(ws);
    const size_t valuePos = commaPos + 1 + valueOffset;

    const size_t wsBeforeInOffset = line.substr(valuePos).find_first_of(ws);
    const size_t wsBeforeInPos = valuePos + wsBeforeInOffset;

    valueName = trim(line.substr(valuePos, wsBeforeInOffset));

    const size_t inOffset = line.substr(wsBeforeInPos).find_first_not_of(ws);
    const size_t inPos = wsBeforeInPos + inOffset;

    if (line.substr(inPos).length() < inString.length() + 1 ||
        line.substr(inPos, inString.length()) != inString)
    {
        throw ParserError("Expected 'in' in #for statement. " + debugString(env));
    }

    const size_t wsBeforeDictionaryPos = inPos + inString.length();
    const size_t dictionaryOffset =
        line.substr(wsBeforeDictionaryPos).find_first_not_of(ws);
    const size_t dictionaryPos = wsBeforeDictionaryPos + dictionaryOffset;

    const size_t endOffset = line.substr(dictionaryPos).find_first_of(ws);
    dictionaryName = trim(line.substr(dictionaryPos, endOffset));

    return true;
}

bool ShaderPreprocessor::parseRange(const std::string& dictionaryName,
                                    Dictionary& dictionary, int& min, int& max)
{
    static const std::string twoDots = "..";
    const size_t minStart = 0;
    const size_t minEnd = dictionaryName.find(twoDots);

    if (minEnd == std::string::npos) {
        throw ParserError("Expected '..' in range. " + dictionaryName);
    }
    const int minimum = std::stoi(dictionaryName.substr(minStart, minEnd - minStart));

    const size_t maxStart = minEnd + 2;
    const size_t maxEnd = dictionaryName.length();

    const int maximum = std::stoi(dictionaryName.substr(maxStart, maxEnd - maxStart));

    // Create all the elements in the dictionary
    for (int i = 0; i <= maximum - minimum; i++) {
        dictionary.setValue(std::to_string(i + 1), std::to_string(minimum + i));
    }

    // Everything went well. Write over min and max
    min = minimum;
    max = maximum;

    return true;
}

bool ShaderPreprocessor::parseFor(ShaderPreprocessor::Env& env) {
    std::string keyName;
    std::string valueName;
    std::string dictionaryName;
    if (!tokenizeFor(env.line, keyName, valueName, dictionaryName, env)) {
         return false;
     }

    if (keyName.empty()) {
        // No key means that the for statement could possibly be a range.
        Dictionary rangeDictionary;
        int min = 0;
        int max = 0;
        if (!parseRange(dictionaryName, rangeDictionary, min, max)) {
            return false;
        }
        // Previous dictionary name is not valid as a key since it has dots in it.
        dictionaryName = std::format("(Range {} to {})", min, max);
        // Add the inner dictionary
        _dictionary.setValue(dictionaryName, rangeDictionary);
    }

    // The dictionary name can be an alias.
    // Resolve the real dictionary reference.
    std::string dictionaryRef;
    if (!resolveAlias(dictionaryName, dictionaryRef, env)) {
        throw SubstitutionError(std::format(
            "Could not resolve variable '{}'. {}", dictionaryName, debugString(env)
        ));
    }
    // Fetch the dictionary to iterate over.
    const Dictionary innerDictionary = _dictionary.value<Dictionary>(dictionaryRef);

    std::vector<std::string_view> keys = innerDictionary.keys();
    ShaderPreprocessor::Input& input = env.inputs.back();
    int keyIndex = 0;

    std::map<std::string, std::string> table;
    if (!keys.empty()) {
        table[keyName] = "\"" + std::string(keys[0]) + "\"";
        table[valueName] = dictionaryRef + "." + std::string(keys[0]);
        keyIndex = 0;

        env.output << "//# For loop over " << dictionaryRef << '\n';
        env.output << "//# Key " << keys[0] << " in " << dictionaryRef << '\n';
        addLineNumber(env);
    }
    else {
        keyIndex = -1;
        env.output << "//# Empty for loop\n";
    }
    pushScope(table, env);

    env.forStatements.push_back({
        static_cast<unsigned int>(env.inputs.size() - 1),
        input.lineNumber,
        static_cast<unsigned int>(input.stream.tellg()),
        keyName,
        valueName,
        dictionaryRef,
        keyIndex
    });

    return true;
}

bool ShaderPreprocessor::parseEndFor(ShaderPreprocessor::Env& env) {
    static const std::string endForString = "#endfor";

    const std::string& line = env.line;
    const size_t length = line.length();

    if (length <= 6 || line.substr(0, endForString.length()) != endForString) {
        return false;
    }

    if (env.forStatements.empty()) {
        throw ParserError(
            "Unexpected #endfor. No corresponding #for was found" + debugString(env)
        );
    }

    ForStatement& forStmnt = env.forStatements.back();
    // Require #for and #endfor to be in the same input file
    if (forStmnt.inputIndex != env.inputs.size() - 1) {
        env.success = false;
        const int inputIndex = forStmnt.inputIndex;
        const ShaderPreprocessor::Input& forInput = env.inputs[inputIndex];
        std::filesystem::path path = forInput.file.path();
        int lineNumber = forStmnt.lineNumber;

        throw ParserError(std::format(
            "Unexpected #endfor. Last #for was in {}: {}. {}",
            path.string(), lineNumber, debugString(env)
        ));
    }

    popScope(env);
    forStmnt.keyIndex++;

    // Fetch the dictionary to iterate over
    const Dictionary innerDict = _dictionary.value<Dictionary>(
        forStmnt.dictionaryReference
    );
    std::vector<std::string_view> keys = innerDict.keys();

    std::map<std::string, std::string> table;
    if (forStmnt.keyIndex < static_cast<int>(keys.size())) {
        std::string_view key = keys[forStmnt.keyIndex];
        table[forStmnt.keyName] = std::format("\"{}\"", key);
        table[forStmnt.valueName] = std::format(
            "{}.{}", forStmnt.dictionaryReference, key
        );
        pushScope(table, env);
        env.output <<
            std::format("//# Key {} in {}\n", key, forStmnt.dictionaryReference);
        addLineNumber(env);
        // Restore input to its state from when #for was found
        Input& input = env.inputs.back();
        input.stream.seekg(forStmnt.streamPos);
        input.lineNumber = forStmnt.lineNumber;
    }
    else {
        // This was the last iteration (or there ware zero iterations)
        env.output <<
            std::format("//# Terminated loop over {}\n", forStmnt.dictionaryReference);
        addLineNumber(env);
        env.forStatements.pop_back();
    }
    return true;
}

} // namespace ghoul::opengl
