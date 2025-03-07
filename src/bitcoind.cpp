// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <chainparams.h>
#include <clientversion.h>
#include <compat.h>
#include <fs.h>
#include <init.h>
#include <interfaces/chain.h>
#include <noui.h>
#include <shutdown.h>
#include <ui_interface.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/threadnames.h>
#include <util/translation.h>

#include <functional>

const std::function<std::string(const char*)> G_TRANSLATION_FUN = nullptr;

static void WaitForShutdown()
{
    while (!ShutdownRequestedMainThread())
    {
        MilliSleep(200);
    }
    Interrupt();
}

#if ENABLE_ZMQ
extern int GetNewZMQKeypair(char *server_public_key, char *server_secret_key);
#endif

//////////////////////////////////////////////////////////////////////////////
//
// Start
//
static bool AppInit(int argc, char* argv[])
{
    InitInterfaces interfaces;
    interfaces.chain = interfaces::MakeChain();

    bool fRet = false;

    util::ThreadRename("init");

    //
    // Parameters
    //
    // If Qt is used, parameters/bitcoin.conf are parsed in qt/bitcoin.cpp's main()
    SetupServerArgs();
    std::string error;
    if (!gArgs.ParseParameters(argc, argv, error)) {
        return InitError(strprintf("Error parsing command line arguments: %s\n", error));
    }

    // Process help and version before taking care about datadir
    if (HelpRequested(gArgs) || gArgs.IsArgSet("-version")) {
        std::string strUsage = PACKAGE_NAME " version " + FormatFullVersion() + "\n";

        if (gArgs.IsArgSet("-version"))
        {
            strUsage += FormatParagraph(LicenseInfo()) + "\n";
        }
        else
        {
            strUsage += "\nUsage:  particld [options]                     Start " PACKAGE_NAME "\n";
            strUsage += "\n" + gArgs.GetHelpMessage();
        }

        tfm::format(std::cout, "%s", strUsage.c_str());
        return true;
    }

#if ENABLE_ZMQ
    if (gArgs.IsArgSet("-newserverkeypairzmq")) {
        std::string sOut;
        char server_public_key[41], server_secret_key[41];
        if (0 != GetNewZMQKeypair(server_public_key, server_secret_key)) {
            tfm::format(std::cerr, "zmq_curve_keypair failed.\n");
            return true;
        }
        sOut = "Server Public key:      " + std::string(server_public_key) + "\n"
             + "Server Secret key:      " + std::string(server_secret_key) + "\n"
             + "Server Secret key b64:  " + EncodeBase64((uint8_t*)server_secret_key, 40) + "\n";

        tfm::format(std::cout, "%s", sOut.c_str());
        return true;
    }
#endif

    try
    {
        if (!CheckDataDirOption()) {
            return InitError(strprintf("Specified data directory \"%s\" does not exist.\n", gArgs.GetArg("-datadir", "")));
        }
        if (!gArgs.ReadConfigFiles(error, true)) {
            return InitError(strprintf("Error reading configuration file: %s\n", error));
        }
        // Check for -chain, -testnet or -regtest parameter (Params() calls are only valid after this clause)
        try {
            SelectParams(gArgs.GetChainName());
        } catch (const std::exception& e) {
            return InitError(strprintf("%s\n", e.what()));
        }

        // Error out when loose non-argument tokens are encountered on command line
        for (int i = 1; i < argc; i++) {
            if (!IsSwitchChar(argv[i][0])) {
                return InitError(strprintf("Command line contains unexpected token '%s', see particld -h for a list of options.\n", argv[i]));
                exit(EXIT_FAILURE);
            }
        }

        // -server defaults to true for bitcoind but not for the GUI so do this here
        gArgs.SoftSetBoolArg("-server", true);
        // Set this early so that parameter interactions go to console
        InitLogging();
        InitParameterInteraction();
        if (!AppInitBasicSetup())
        {
            // InitError will have been called with detailed error, which ends up on console
            return false;
        }
        if (!AppInitParameterInteraction())
        {
            // InitError will have been called with detailed error, which ends up on console
            return false;
        }
        if (!AppInitSanityChecks())
        {
            // InitError will have been called with detailed error, which ends up on console
            return false;
        }
        if (gArgs.GetBoolArg("-daemon", false))
        {
#if HAVE_DECL_DAEMON
#if defined(MAC_OSX)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
            tfm::format(std::cout, PACKAGE_NAME " starting\n");

            // Daemonize
            if (daemon(1, 0)) { // don't chdir (1), do close FDs (0)
                return InitError(strprintf("daemon() failed: %s\n", strerror(errno)));
            }
#if defined(MAC_OSX)
#pragma GCC diagnostic pop
#endif
#else
            return InitError("-daemon is not supported on this operating system\n");
#endif // HAVE_DECL_DAEMON
        }
        // Lock data directory after daemonization
        if (!AppInitLockDataDirectory())
        {
            // If locking the data directory failed, exit immediately
            return false;
        }

#ifdef WIN32
        if (CreateMessageWindow() != 0) {
            return false;
        }
#endif

        fRet = AppInitMain(interfaces);
    }
    catch (const std::exception& e) {
        PrintExceptionContinue(&e, "AppInit()");
    } catch (...) {
        PrintExceptionContinue(nullptr, "AppInit()");
    }

    if (!fRet)
    {
        Interrupt();
    } else {
        WaitForShutdown();
    }
    Shutdown(interfaces);

    return fRet;
}

int main(int argc, char* argv[])
{
#ifdef WIN32
    util::WinCmdLineArgs winArgs;
    std::tie(argc, argv) = winArgs.get();
#endif
    SetupEnvironment();

    // Connect bitcoind signal handlers
    noui_connect();

    return (AppInit(argc, argv) ? EXIT_SUCCESS : EXIT_FAILURE);
}
