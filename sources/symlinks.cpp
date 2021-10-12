
#include <memory>
#include <string>
#include <set>
#include <Windows.h>
using namespace std;

int wmain(int argc, wchar_t *argv[])
{
	auto size = 1 << 12;
	unique_ptr<wchar_t[]> buffer;
	for (;;)
	{
		buffer = make_unique<wchar_t[]>(size);
		if (0 == QueryDosDevice(nullptr, buffer.get(), size))
		{
			if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
			{
				size = size * 2;
				continue;
			}
			else
			{
				printf("Error: %d\n", GetLastError());
				return 1;
			}
		}
		else
		{
			break;
		}
	}
	if (argc > 1)
	{
		_wcslwr_s(argv[1], wcslen(argv[1]) + 1);
	}

	auto filter = argc > 1 ? argv[1] : nullptr;

	// simplify stored type
	using LinkPair = pair<wstring, wstring>;

	struct LessNoCase
	{
		bool operator()(const LinkPair &p1, const LinkPair &p2) const
		{
			return _wcsicmp(p1.first.c_str(), p2.first.c_str()) > 0;
		}
	};

	// sorted by LessNoCase
	set<LinkPair, LessNoCase> links;
	WCHAR target[512];

	for (auto p = buffer.get(); *p;)
	{
		wstring name(p);
		auto locase(name);
		_wcslwr_s((WCHAR *)locase.data(), locase.size() + 1);
		if (filter == nullptr || locase.find(filter) != wstring::npos)
		{
			QueryDosDevice(name.c_str(), target, _countof(target));
			// add pair to results
			links.insert({name, target});
		}
		// move to next item
		p += name.size() + 1;
	}

	// print results
	for (auto &link : links)
	{
		printf("%ws = %ws\n", link.first.c_str(), link.second.c_str());
	}
}
