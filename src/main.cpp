#include <AttnServer.h>
//#include <RPCServer.h>

#include <ripple/basics/base64.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/beast/core/SemanticVersion.h>
#include <ripple/beast/unit_test.h>
#include <ripple/beast/unit_test/dstream.hpp>

#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <boost/preprocessor/stringize.hpp>

#ifdef BOOST_MSVC
#include <Windows.h>
#endif

//------------------------------------------------------------------------------
//  The build version number. You must edit this for each release
//  and follow the format described at http://semver.org/
//--------------------------------------------------------------------------
char const* const versionString = "0.1.0"

#if defined(DEBUG) || defined(SANITIZER)
    "+"
#ifdef DEBUG
    "DEBUG"
#ifdef SANITIZER
    "."
#endif
#endif

#ifdef SANITIZER
    BOOST_PP_STRINGIZE(SANITIZER)
#endif
#endif

    //--------------------------------------------------------------------------
    ;


//LCOV_EXCL_START
void printHelp(const boost::program_options::options_description& desc)
{
    std::cerr
        << "attn_server [options] <command> [<argument> ...]\n"
        << desc << std::endl
        << "Commands: \n"
#if 0
           "     create_keys                   Generate validator keys.\n"
           "     create_token                  Generate validator token.\n"
           "     revoke_keys                   Revoke validator keys.\n"
           "     sign <data>                   Sign string with validator key.\n"
           "     show_manifest [hex|base64]    Displays the last generated manifest\n"
           "     set_domain <domain>           Associate a domain with the validator key.\n"
           "     clear_domain                  Disassociate a domain from a validator key.\n"
           "     attest_domain                 Produce the attestation string for a domain.\n"
#endif
        ;
}
//LCOV_EXCL_STOP

std::string const& getVersionString()
{
    static std::string const value = [] {
        std::string const s = versionString;
        beast::SemanticVersion v;
        if (!v.parse(s) || v.print() != s)
            throw std::logic_error(s + ": Bad version string"); //LCOV_EXCL_LINE
        return s;
    }();
    return value;
}

static std::string getEnvVar(char const* name)
{
    auto const v = getenv(name);
    if (v != nullptr)
        return { v };
    return {};
}

static int runUnitTests()
{
    using namespace beast::unit_test;
    beast::unit_test::dstream dout{std::cout};
    reporter r{dout};
    bool const anyFailed = r.run_each(global_suites());
    if(anyFailed)
        return EXIT_FAILURE;    //LCOV_EXCL_LINE
    return EXIT_SUCCESS;
}

int main(int argc, char** argv)
{
    namespace po = boost::program_options;

    po::variables_map vm;

    // Set up option parsing.
    //
    po::options_description general ("General Options");
    general.add_options()
    ("help,h", "Display this message.")
    ("config", po::value<std::string>(), "Specify the config file.")
    ("request", po::value<std::string>(), "Execute provided request and outputs json result")
    ("unittest,u", "Perform unit tests.")
    ("version", "Display the build version.")
    ;

    po::options_description hidden("Hidden options");
    hidden.add_options()
    ("command",   po::value< std::string >(), "Command.")
    ("arguments", po::value< std::vector<std::string> >()->default_value(
        std::vector <std::string>(), "empty"), "Arguments.")
    ;
    po::positional_options_description p;
    p.add("command", 1).add("arguments", -1);

    po::options_description cmdline_options;
    cmdline_options.add(general).add(hidden);

    // Parse options, if no error.
    try
    {
        po::store(po::command_line_parser(argc, argv)
            .options(cmdline_options)    // Parse options.
            .positional(p)
            .run(),
            vm);
        po::notify(vm);                  // Invoke option notify functions.
    }
    //LCOV_EXCL_START
    catch (std::exception const&)
    {
        std::cerr << "attn_server: Incorrect command line syntax." << std::endl;
        std::cerr << "Use '--help' for a list of options." << std::endl;
        return EXIT_FAILURE;
    }
    //LCOV_EXCL_STOP

    // Run the unit tests if requested.
    // The unit tests will exit the application with an appropriate return code.
    if (vm.count("unittest"))
        return runUnitTests();

    //LCOV_EXCL_START
    if (vm.count("version"))
    {
        std::cout << "attn_server version " << getVersionString() << std::endl;
        return 0;
    }

    if (vm.count("help"))
    {
        printHelp(general);
        return EXIT_SUCCESS;
    }

    try
    {
        std::string config_filename;
        if (vm.count("config"))
            config_filename = vm["config"].as<std::string>();
        else {
            config_filename = getEnvVar("h");
            config_filename += "/github/ripple/attn_server/attn_srv.json";
        }
        ripple::sidechain::AttnServer attn_server(config_filename);
        if (vm.count("request")) {
            // ex: --request "{\"request\": { \"door_account\": \"rEEw6AmPHD28M5AHyrzFSVoLA3oANcmYas\", \"tx_hash\": \"D638208ADBD04CBB10DE7B645D3AB4BA31489379411A3A347151702B6401AA78\"} }"
            
            // ex:  --request "{\"request\": { \"dst_door\": \"rEEw6AmPHD28M5AHyrzFSVoLA3oANcmYas\", \"sidechain\": {\"src_chain_door\" : \"rEEw6AmPHD28M5AHyrzFSVoLA3oANcmYas\", \"src_chain_issue\": \"XRP\", \"dst_chain_door\" :  \"rEEw6AmPHD28M5AHyrzFSVoLA3oANcmYas\", \"dst_chain_issue\": \"XRP\"}, \"amount\":  { \"value\": \"13.1\", \"currency\": \"FOO\", \"issuer\": \"rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn\" }, \"xchain_sequence_number\": \"22\"} }"
            
            std::string request { vm["request"].as<std::string>() };
            auto res = attn_server.process_rpc_request(request);
            std::cout << res << std::endl;
            return EXIT_SUCCESS;
        }
        attn_server.mainLoop();
    }
    catch(std::exception const& e)
    {
        std::cerr << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
    //LCOV_EXCL_STOP
}
