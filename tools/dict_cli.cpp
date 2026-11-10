// Command-line access to the engine for validating real dictionaries outside the GUI.
//   dr-dict-cli <data-dir> add <file-or-folder>...
//   dr-dict-cli <data-dir> list | folders | rescan | remove <dict-id> | suggest <prefix> | lookup <word>
//   dr-dict-cli <data-dir> variants <chinese-word> [t|tw|twp|hk|all]
//   dr-dict-cli <data-dir> article <dict-id> <word> | resource <dict-id> <path> [save-as]
#include "dr/library.h"
#include "dr/text.h"
#include <chrono>
#include <iostream>
#include <windows.h>
using namespace dr;
int wmain(int argc, wchar_t **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3)
    {
        std::cerr << "usage: dr-dict-cli <data-dir> <command> [args]\n";
        return 2;
    }
    try
    {
        Library library(argv[1]);
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto opencc = fs::path(executable).parent_path() / "opencc";
        library.set_opencc_dir(opencc);
        auto command = narrow(argv[2]);
        auto arg = [&](int i) { return i < argc ? narrow(argv[i]) : std::string(); };
        auto started = std::chrono::steady_clock::now();
        if (command == "add")
        {
            std::vector<fs::path> paths;
            for (int i = 3; i < argc; ++i)
                paths.emplace_back(argv[i]);
            auto result = library.add(paths, [](const Json &p) {
                std::cerr << "\r" << p.value("name", "") << " " << int(p.value("fraction", 0.0) * 100) << "%   ";
            });
            std::cerr << "\n";
            result.erase("dictionaries");
            std::cout << result.dump(2) << "\n";
        }
        else if (command == "list")
            std::cout << library.list().dump(2) << "\n";
        else if (command == "remove")
            library.remove(arg(3));
        else if (command == "folders")
            std::cout << library.folders().dump(2) << "\n";
        else if (command == "rescan")
        {
            auto result = library.rescan({});
            result.erase("dictionaries");
            std::cout << result.dump(2) << "\n";
        }
        else if (command == "suggest")
            std::cout << library.suggest(arg(3), 20).dump(2) << "\n";
        else if (command == "lookup")
            std::cout << library.lookup(arg(3)).dump(2) << "\n";
        else if (command == "variants")
        {
            // Other writings of a Chinese word, independent of the saved settings.
            ChineseVariants variants(opencc);
            std::cout << Json(variants.expand(arg(3), argc > 4 ? arg(4) : "all")).dump(2) << "\n";
        }
        else if (command == "article")
            std::cout << library.article_page(arg(3), arg(4), "light") << "\n";
        else if (command == "resource")
        {
            auto r = library.resource(arg(3), arg(4));
            if (!r)
            {
                std::cerr << "not found\n";
                return 1;
            }
            std::cout << r->mime << " " << r->data.size() << " bytes\n";
            if (argc > 5)
                write_file(argv[5], r->data);
        }
        else
        {
            std::cerr << "unknown command\n";
            return 2;
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        std::cerr << "(" << ms.count() << " ms)\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
