// $Id$

#include "RomDatabase.hh"
#include "RomInfo.hh"
#include "Rom.hh"
#include "InfoTopic.hh"
#include "CommandException.hh"
#include "TclObject.hh"
#include "FileContext.hh"
#include "FileException.hh"
#include "FileOperations.hh"
#include "LocalFileReference.hh"
#include "GlobalCommandController.hh"
#include "CliComm.hh"
#include "StringOp.hh"
#include "XMLLoader.hh"
#include "XMLElement.hh"
#include "XMLException.hh"
#include <map>
#include <cassert>

using std::auto_ptr;
using std::map;
using std::set;
using std::string;
using std::vector;

// Note: Using something like boost::shared_ptr would simplify memory management
//       but it does add another dependency. So atm we don't use it yet.

namespace openmsx {

typedef map<string, RomInfo*, StringOp::caseless> DBMap;
static DBMap romDBSHA1;
typedef map<string, unsigned> UnknownTypes;
UnknownTypes unknownTypes;

class SoftwareInfoTopic : public InfoTopic
{
public:
        explicit SoftwareInfoTopic(InfoCommand& openMSXInfoCommand, RomDatabase& romDatabase);

        virtual void execute(const std::vector<TclObject*>& tokens,
                             TclObject& result) const;
        virtual std::string help(const std::vector<std::string>& tokens) const;
        virtual void tabCompletion(std::vector<std::string>& tokens) const;

private:
	RomDatabase& romDatabase;
};

RomDatabase::RomDatabase(GlobalCommandController& commandController, CliComm& cliComm)
	: softwareInfoTopic(new SoftwareInfoTopic(commandController.getOpenMSXInfoCommand(), *this))
{
	initDatabase(cliComm);
}

RomDatabase::~RomDatabase()
{
	for (DBMap::const_iterator it = romDBSHA1.begin();
	     it != romDBSHA1.end(); ++it) {
		delete it->second;
	}
}

static string parseRemarks(const XMLElement& elem)
{
	string result;
	XMLElement::Children remarks;
	elem.getChildren("remark", remarks);
	for (XMLElement::Children::const_iterator it = remarks.begin();
	     it != remarks.end(); ++it) {
		const XMLElement& remark = **it;
		XMLElement::Children texts;
		remark.getChildren("text", texts);
		for (XMLElement::Children::const_iterator it = texts.begin();
		     it != texts.end(); ++it) {
			// TODO language attribute is ignored
			result += (*it)->getData() + '\n';
		}
	}
	return result;
}

static void addEntry(CliComm& cliComm, auto_ptr<RomInfo> romInfo,
                     const string& sha1, DBMap& result)
{
	assert(romInfo.get());
	if (result.find(sha1) == result.end()) {
		result[sha1] = romInfo.release();
	} else {
		cliComm.printWarning(
			"duplicate softwaredb entry SHA1: " + sha1);
	}
}

static void parseEntry(CliComm& cliComm,
	const XMLElement& rom, DBMap& result,
	const string& title,   const string& year,
	const string& company, const string& country,
	bool original,         const string& origType,
	const string& remark,  const string& type)
{
	XMLElement::Children hashTags;
	rom.getChildren("hash", hashTags);
	for (XMLElement::Children::const_iterator it2 = hashTags.begin();
	     it2 != hashTags.end(); ++it2) {
		if ((*it2)->getAttribute("algo") != "sha1") {
			continue;
		}
		RomType romType = RomInfo::nameToRomType(type);
		if (romType == ROM_UNKNOWN) {
			unknownTypes[type]++;
		}
		auto_ptr<RomInfo> romInfo(new RomInfo(
			title, year, company, country, original, origType,
			remark, romType));
		string sha1 = (*it2)->getData();
		addEntry(cliComm, romInfo, sha1, result);
	}
}

static string parseStart(const XMLElement& rom)
{
	string start = rom.getChildData("start", "");
	if      (start == "0x0000") return "0000";
	else if (start == "0x4000") return "4000";
	else if (start == "0x8000") return "8000";
	else if (start == "0xC000") return "C000";
	else return "";
}

static void parseDump(CliComm& cliComm,
	const XMLElement& dump, DBMap& result,
	const string& title,   const string& year,
	const string& company, const string& country,
	const string& remark)
{
	// TODO make <original> tag not required?
	const XMLElement& originalTag = dump.getChild("original");
	bool original = originalTag.getAttributeAsBool("value");
	string origType = originalTag.getData();

	if (const XMLElement* megarom = dump.findChild("megarom")) {
		parseEntry(cliComm, *megarom, result, title, year,
			   company, country, original, origType,
			   remark, megarom->getChildData("type"));
	} else if (const XMLElement* rom = dump.findChild("rom")) {
		string type = rom->getChildData("type", "Mirrored");
		if (type == "Normal") {
			type += parseStart(*rom);
		} else if (type == "Mirrored") {
			type += parseStart(*rom);
		}
		parseEntry(cliComm, *rom, result, title, year, company,
		           country, original, origType, remark, type);
	}
}

static void parseSoftware(CliComm& cliComm, const string& filename,
                          const XMLElement& soft, DBMap& result)
{
	try {
		const XMLElement* system = soft.findChild("system");
		if (system && (system->getData() != "MSX")) {
			// skip non-MSX entries
			return;
		}

		// TODO there can be multiple title tags
		string title   = soft.getChildData("title", "");
		string year    = soft.getChildData("year", "");
		string company = soft.getChildData("company", "");
		string country = soft.getChildData("country", "");
		string remark  = parseRemarks(soft);

		XMLElement::Children dumps;
		soft.getChildren("dump", dumps);
		for (XMLElement::Children::const_iterator it = dumps.begin();
		     it != dumps.end(); ++it) {
			parseDump(cliComm, **it, result, title, year,
			          company, country, remark);
		}
	} catch (MSXException& e) {
		string title = soft.getChildData("title", "<missing-title>");
		cliComm.printWarning("Wrong entry with title '" + title +
		                     "' in " + filename + ": " + e.getMessage());
	}
}

static void parseDB(CliComm& cliComm, const string& filename,
                    const XMLElement& doc, DBMap& result)
{
	const XMLElement::Children& children = doc.getChildren();
	for (XMLElement::Children::const_iterator it = children.begin();
	     it != children.end(); ++it) {
		// Parse all <software> tags
		parseSoftware(cliComm, filename, **it, result);
	}
}

static auto_ptr<XMLElement> openDB(CliComm& cliComm, const string& filename,
                                   const string& type)
{
	auto_ptr<XMLElement> doc;
	try {
		LocalFileReference file(filename);
		doc = XMLLoader::load(file.getFilename(), type);
	} catch (FileException&) {
		// couldn't read file
	} catch (XMLException& e) {
		cliComm.printWarning(
			"Could not parse ROM DB: " + e.getMessage() + "\n"
			"Romtype detection might fail because of this.");
	}
	return doc;
}

void RomDatabase::initDatabase(CliComm& cliComm)
{
	static bool init = false;
	if (init) return;
	init = true;

	SystemFileContext context;
	CommandController* controller = NULL; // ok for SystemFileContext
	vector<string> paths = context.getPaths(*controller);
	for (vector<string>::const_iterator it = paths.begin();
	     it != paths.end(); ++it) {
		string filename = FileOperations::join(*it, "softwaredb.xml");
		auto_ptr<XMLElement> doc(
			openDB(cliComm, filename, "softwaredb1.dtd"));
		if (doc.get()) {
			DBMap tmp;
			parseDB(cliComm, filename, *doc, tmp);
			for (DBMap::const_iterator it = tmp.begin();
			     it != tmp.end(); ++it) {
				if (romDBSHA1.find(it->first) == romDBSHA1.end()) {
					// new entry
					romDBSHA1.insert(*it);
				} else {
					// duplicate entry
					delete it->second;
				}
			}
		}
	}
	if (romDBSHA1.empty()) {
		cliComm.printWarning(
			"Couldn't load software database.\n"
			"This may cause incorrect ROM mapper types to be used.");
	}
	if (!unknownTypes.empty()) {
		StringOp::Builder output;
		output << "Unknown mapper types in software database: ";
		for (UnknownTypes::iterator it = unknownTypes.begin();
		     it != unknownTypes.end(); ++it) {
			output << it->first << " (" << it->second << "x); ";
		}
		cliComm.printWarning(output);
	}
}

const RomInfo* RomDatabase::fetchRomInfo(const string& sha1sum) const
{
	// Note: RomInfo is copied only to make ownership managment easier

	if (romDBSHA1.find(sha1sum) != romDBSHA1.end()) {
		// Return a copy of the DB entry.
		return romDBSHA1[sha1sum];
	}

	// no match found
	return NULL;
}

// SoftwareInfoTopic

SoftwareInfoTopic::SoftwareInfoTopic(InfoCommand& openMSXInfoCommand, RomDatabase& romDatabase_)
	: InfoTopic(openMSXInfoCommand, "software")
	, romDatabase(romDatabase_)
{
}

void SoftwareInfoTopic::execute(const vector<TclObject*>& tokens,
		TclObject& result) const
{
	if (tokens.size() != 3) {
		throw CommandException("Wrong number of parameters");
	}

	const string& sha1sum = tokens[2]->getString();
	const RomInfo* romInfo = romDatabase.fetchRomInfo(sha1sum);
	if (romInfo == NULL) {
		// no match found
		throw CommandException("Software with sha1sum " + sha1sum + " not found");
	}

	result.addListElement("title");
	result.addListElement(romInfo->getTitle());
	result.addListElement("year");
	result.addListElement(romInfo->getYear());
	result.addListElement("company");
	result.addListElement(romInfo->getCompany());
	result.addListElement("country");
	result.addListElement(romInfo->getCountry());
	result.addListElement("orig_type");
	result.addListElement(romInfo->getOrigType());
	result.addListElement("remark");
	result.addListElement(romInfo->getRemark());
	result.addListElement("original");
	result.addListElement(romInfo->getOriginal());
	result.addListElement("mapper_type_name");
	result.addListElement(RomInfo::romTypeToName(romInfo->getRomType()));
	return;
}

string SoftwareInfoTopic::help(const vector<string>& /*tokens*/) const
{
	return "Gives information about the software "
		"given its sha1sum, in a paired list.";
}

void SoftwareInfoTopic::tabCompletion(vector<string>& /*tokens*/) const
{
	// no useful completion possible
}

} // namespace openmsx
