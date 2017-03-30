/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @author Alex Beregszaszi
 * @date 2016
 * Standard JSON compiler interface.
 */

#include <libsolidity/interface/StandardCompiler.h>
#include <libsolidity/interface/SourceReferenceFormatter.h>
#include <libsolidity/ast/ASTJsonConverter.h>
#include <libevmasm/Instruction.h>
#include <libdevcore/JSON.h>

using namespace std;
using namespace dev;
using namespace dev::solidity;

namespace {

Json::Value formatError(
	bool _warning,
	string const& _type,
	string const& _component,
	string const& _message,
	string const& _formattedMessage = "",
	Json::Value const& _sourceLocation = Json::Value()
)
{
	Json::Value error = Json::objectValue;
	error["type"] = _type;
	error["component"] = _component;
	error["severity"] = _warning ? "warning" : "error";
	error["message"] = _message;
	error["formattedMessage"] = (_formattedMessage.length() > 0) ? _formattedMessage : _message;
	if (_sourceLocation.isObject())
		error["sourceLocation"] = _sourceLocation;
	return error;
}

Json::Value formatFatalError(string const& _type, string const& _message)
{
	Json::Value output = Json::objectValue;
	output["errors"] = Json::arrayValue;
	output["errors"].append(formatError(false, _type, "general", _message));
	return output;
}

Json::Value formatErrorWithException(
	Exception const& _exception,
	bool const& _warning,
	string const& _type,
	string const& _component,
	string const& _message,
	function<Scanner const&(string const&)> const& _scannerFromSourceName
)
{
	string message;
	string formattedMessage = SourceReferenceFormatter::formatExceptionInformation(_exception, _message, _scannerFromSourceName);

	// NOTE: the below is partially a copy from SourceReferenceFormatter
	SourceLocation const* location = boost::get_error_info<errinfo_sourceLocation>(_exception);

	if (string const* description = boost::get_error_info<errinfo_comment>(_exception))
		message = ((_message.length() > 0) ? (_message + ":") : "") + *description;
	else
		message = _message;

	if (location && location->sourceName)
	{
		Json::Value sourceLocation = Json::objectValue;
		sourceLocation["file"] = *location->sourceName;
		sourceLocation["start"] = location->start;
		sourceLocation["end"] = location->end;
	}

	return formatError(_warning, _type, _component, message, formattedMessage, location);
}

StringMap createSourceList(Json::Value const& _input)
{
	StringMap sources;
	Json::Value const& jsonSources = _input["sources"];
	if (jsonSources.isObject())
		for (auto const& sourceName: jsonSources.getMemberNames())
			sources[sourceName] = jsonSources[sourceName]["content"].asString();
	return sources;
}

Json::Value methodIdentifiers(ContractDefinition const& _contract)
{
	Json::Value methodIdentifiers(Json::objectValue);
	for (auto const& it: _contract.interfaceFunctions())
		methodIdentifiers[it.second->externalSignature()] = toHex(it.first.ref());
	return methodIdentifiers;
}

Json::Value formatLinkReferences(std::map<size_t, std::string> const& linkReferences)
{
	Json::Value ret(Json::objectValue);

	for (auto const& ref: linkReferences)
	{
		string const& fullname = ref.second;
		size_t colon = fullname.find(':');
		solAssert(colon != string::npos, "");
		string file = fullname.substr(0, colon);
		string name = fullname.substr(colon + 1);

		Json::Value fileObject = ret.get(file, Json::objectValue);
		Json::Value libraryArray = fileObject.get(name, Json::arrayValue);

		Json::Value entry = Json::objectValue;
		entry["start"] = Json::UInt(ref.first);
		entry["length"] = 20;

		libraryArray.append(entry);
		fileObject[name] = libraryArray;
		ret[file] = fileObject;
	}

	return ret;
}

Json::Value collectEVMObject(eth::LinkerObject const& _object, string const* _sourceMap)
{
	Json::Value output = Json::objectValue;
	output["object"] = _object.toHex();
	output["opcodes"] = solidity::disassemble(_object.bytecode);
	output["sourceMap"] = _sourceMap ? *_sourceMap : "";
	output["linkReferences"] = formatLinkReferences(_object.linkReferences);
	return output;
}

bool isTargetRequired(Json::Value const& _targets, string const& _target)
{
	for (auto const& target: _targets)
		if (target == _target)
			return true;
	return false;
}

///
/// @_targets is a JSON object containining a two-level hashmap, where the first level is the filename,
/// the second level is the contract name and the value is an array of target names to be requested for that contract.
/// @_file is the current file
/// @_contract is the current contract
/// @_target is the current target name
///
/// Returns true if the _targets has a match for the requested target in the specific file / contract.
///
/// In @_targets the use of '*' as a wildcard is permitted.
///
/// TODO: optimise this. Perhaps flatten the structure upfront.
bool isTargetRequired(Json::Value const& _targets, string const& _file, string const& _contract, string const& _target)
{
	if (!_targets.isObject())
		return false;

	for (auto const& file: { string("*"), _file })
	{
		if (_targets[file].isObject())
		{
			if (isTargetRequired(_targets[_file][_contract], _target))
				return true;
			if (isTargetRequired(_targets[_file]["*"], _target))
				return true;
			if (isTargetRequired(_targets[_file][""], _target))
				return true;
		}
	}

	return false;
}

}

Json::Value StandardCompiler::compileInternal(Json::Value const& _input)
{
	m_compilerStack.reset(false);

	if (!_input.isObject())
		return formatFatalError("JSONError", "Input is not a JSON object.");

	if (_input["language"] != "Solidity")
		return formatFatalError("JSONError", "Only \"Solidity\" is supported as a language.");

	Json::Value const& sources = _input["sources"];
	if (!sources)
		return formatFatalError("JSONError", "No input sources specified.");

	Json::Value errors = Json::arrayValue;

	for (auto const& sourceName: sources.getMemberNames())
		if (sources[sourceName]["content"].isString())
			m_compilerStack.addSource(sourceName, sources[sourceName]["content"].asString());
		else if (sources[sourceName]["urls"].isArray())
		{
			if (!m_readFile)
				return formatFatalError("JSONError", "No import callback supplied, but URL is requested.");

			bool found = false;
			vector<string> failures;

			for (auto const& url: sources[sourceName]["urls"])
			{
				ReadFile::Result result = m_readFile(url.asString());
				if (result.success)
				{
					m_compilerStack.addSource(sourceName, result.contentsOrErrorMessage);
					found = true;
					break;
				}
				else
					failures.push_back("Cannot import url (\"" + url.asString() + "\"): " + result.contentsOrErrorMessage);
			}

			for (auto const& failure: failures)
			{
				/// If the import succeeded, let mark all the others as warnings, otherwise all of them are errors.
				errors.append(formatError(
					found ? true : false,
					"IOError",
					"general",
					failure
				));
			}
		}
		else
			return formatFatalError("JSONError", "Invalid input source specified.");

	Json::Value const& settings = _input.get("settings", Json::Value());

	vector<string> remappings;
	for (auto const& remapping: settings.get("remappings", Json::Value()))
		remappings.push_back(remapping.asString());
	m_compilerStack.setRemappings(remappings);

	Json::Value optimizerSettings = settings.get("optimizer", Json::Value());
	bool optimize = optimizerSettings.get("enabled", Json::Value(false)).asBool();
	unsigned optimizeRuns = optimizerSettings.get("runs", Json::Value(200u)).asUInt();

	map<string, h160> libraries;
	Json::Value jsonLibraries = settings.get("libraries", Json::Value());
	for (auto const& sourceName: jsonLibraries.getMemberNames())
	{
		auto const& jsonSourceName = jsonLibraries[sourceName];
		for (auto const& library: jsonSourceName.getMemberNames())
			// @TODO use libraries only for the given source
			libraries[library] = h160(jsonSourceName[library].asString());
	}

	Json::Value metadataSettings = settings.get("metadata", Json::Value());
	m_compilerStack.useMetadataLiteralSources(metadataSettings.get("useLiteralContent", Json::Value(false)).asBool());

	auto scannerFromSourceName = [&](string const& _sourceName) -> solidity::Scanner const& { return m_compilerStack.scanner(_sourceName); };

	bool success = false;

	try
	{
		success = m_compilerStack.compile(optimize, optimizeRuns, libraries);

		for (auto const& error: m_compilerStack.errors())
		{
			auto err = dynamic_pointer_cast<Error const>(error);

			errors.append(formatErrorWithException(
				*error,
				err->type() == Error::Type::Warning,
				err->typeName(),
				"general",
				"",
				scannerFromSourceName
			));
		}
	}
	catch (Error const& _error)
	{
		if (_error.type() == Error::Type::DocstringParsingError)
			errors.append(formatError(
				false,
				"DocstringParsingError",
				"general",
				"Documentation parsing error: " + *boost::get_error_info<errinfo_comment>(_error)
			));
		else
			errors.append(formatErrorWithException(
				_error,
				false,
				_error.typeName(),
				"general",
				"",
				scannerFromSourceName
			));
	}
	catch (CompilerError const& _exception)
	{
		errors.append(formatErrorWithException(
			_exception,
			false,
			"CompilerError",
			"general",
			"Compiler error (" + _exception.lineInfo() + ")",
			scannerFromSourceName
		));
	}
	catch (InternalCompilerError const& _exception)
	{
		errors.append(formatErrorWithException(
			_exception,
			false,
			"InternalCompilerError",
			"general",
			"Internal compiler error (" + _exception.lineInfo() + ")", scannerFromSourceName
		));
	}
	catch (UnimplementedFeatureError const& _exception)
	{
		errors.append(formatErrorWithException(
			_exception,
			false,
			"UnimplementedFeatureError",
			"general",
			"Unimplemented feature (" + _exception.lineInfo() + ")",
			scannerFromSourceName));
	}
	catch (Exception const& _exception)
	{
		errors.append(formatError(
			false,
			"Exception",
			"general",
			"Exception during compilation: " + boost::diagnostic_information(_exception)
		));
	}
	catch (...)
	{
		errors.append(formatError(
			false,
			"Exception",
			"general",
			"Unknown exception during compilation."
		));
	}

	Json::Value output = Json::objectValue;

	if (errors.size() > 0)
		output["errors"] = errors;

	/// Inconsistent state - stop here to receive error reports from users
	if (!success && (errors.size() == 0))
		return formatFatalError("InternalCompilerError", "No error reported, but compilation failed.");

	output["sources"] = Json::objectValue;
	unsigned sourceIndex = 0;
	for (auto const& source: m_compilerStack.sourceNames())
	{
		Json::Value sourceResult = Json::objectValue;
		sourceResult["id"] = sourceIndex++;
		sourceResult["legacyAST"] = ASTJsonConverter(m_compilerStack.ast(source), m_compilerStack.sourceIndices()).json();
		output["sources"][source] = sourceResult;
	}

	Json::Value contractsOutput = Json::objectValue;
	for (string const& contractName: m_compilerStack.contractNames())
	{
		size_t colon = contractName.find(':');
		solAssert(colon != string::npos, "");
		string file = contractName.substr(0, colon);
		string name = contractName.substr(colon + 1);

		// ABI, documentation and metadata
		Json::Value contractData(Json::objectValue);
		contractData["abi"] = dev::jsonCompactPrint(m_compilerStack.metadata(contractName, DocumentationType::ABIInterface));
		contractData["metadata"] = m_compilerStack.onChainMetadata(contractName);
		contractData["userdoc"] = dev::jsonCompactPrint(m_compilerStack.metadata(contractName, DocumentationType::NatspecUser));
		contractData["devdoc"] = dev::jsonCompactPrint(m_compilerStack.metadata(contractName, DocumentationType::NatspecDev));

		// EVM
		Json::Value evmData(Json::objectValue);
		// @TODO: add ir
		ostringstream tmp;
		m_compilerStack.streamAssembly(tmp, contractName, createSourceList(_input), false);
		evmData["assembly"] = tmp.str();
		evmData["legacyAssembly"] = m_compilerStack.streamAssembly(tmp, contractName, createSourceList(_input), true);
		evmData["methodIdentifiers"] = methodIdentifiers(m_compilerStack.contractDefinition(contractName));
		evmData["gasEstimates"] = m_compilerStack.gasEstimates(contractName);

		evmData["bytecode"] = collectEVMObject(
			m_compilerStack.object(contractName),
			m_compilerStack.sourceMapping(contractName)
		);

		evmData["deployedBytecode"] = collectEVMObject(
			m_compilerStack.runtimeObject(contractName),
			m_compilerStack.runtimeSourceMapping(contractName)
		);

		contractData["evm"] = evmData;

		if (!contractsOutput.isMember(file))
			contractsOutput[file] = Json::objectValue;

		contractsOutput[file][name] = contractData;
	}
	output["contracts"] = contractsOutput;

	{
		ErrorList formalErrors;
		if (m_compilerStack.prepareFormalAnalysis(&formalErrors))
			output["why3"] = m_compilerStack.formalTranslation();

		for (auto const& error: formalErrors)
		{
			auto err = dynamic_pointer_cast<Error const>(error);

			errors.append(formatErrorWithException(
				*error,
				err->type() == Error::Type::Warning,
				err->typeName(),
				"general",
				"",
				scannerFromSourceName
			));
		}

		// FIXME!!
		if (!formalErrors.empty())
			output["errors"] = errors;
	}

	return output;
}

Json::Value StandardCompiler::compile(Json::Value const& _input)
{
	try
	{
		return compileInternal(_input);
	}
	catch (...)
	{
		return formatFatalError("InternalCompilerError", "Internal exception in StandardCompiler::compilerInternal");
	}
}

string StandardCompiler::compile(string const& _input)
{
	Json::Value input;
	Json::Reader reader;

	try
	{
		if (!reader.parse(_input, input, false))
			return jsonCompactPrint(formatFatalError("JSONError", reader.getFormattedErrorMessages()));
	}
	catch(...)
	{
		return "{\"errors\":\"[{\"type\":\"JSONError\",\"component\":\"general\",\"severity\":\"error\",\"message\":\"Error parsing input JSON.\"}]}";
	}

	// cout << "Input: " << input.toStyledString() << endl;
	Json::Value output = compile(input);
	// cout << "Output: " << output.toStyledString() << endl;

	try
	{
		return jsonCompactPrint(output);
	}
	catch(...)
	{
		return "{\"errors\":\"[{\"type\":\"JSONError\",\"component\":\"general\",\"severity\":\"error\",\"message\":\"Error writing output JSON.\"}]}";
	}
}
