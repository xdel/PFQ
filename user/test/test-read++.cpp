#include <cstdio>
#include <cstdlib>

#include <pfq/pfq.hpp>


int
main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s dev\n", argv[0]);
        return 0;
    }

    auto q = pfq::socket(64, 4096);

    q.bind(argv[1]);

    q.enable();

    q.timestamp_enable(true);

    for(;;)
    {
        auto queue = q.read();

        if (queue.size() == 0)
            continue;

        auto it = queue.begin();
        for(; it != queue.end(); ++it)
        {
            while (!it.ready())
                std::this_thread::yield();

            auto h = *it;

            printf("caplen:%d len:%d ifindex:%d hw_queue:%d tstamp: %u:%u [commit:%d]-> ",
					h.caplen, h.len, h.if_index, h.hw_queue,
                    h.tstamp.tv.sec, h.tstamp.tv.nsec, h.commit);

			const char *buff = static_cast<char *>(it.data());

			for(auto x = 0; x < std::min<uint16_t>(h.caplen, 34); x++)
				printf("%2x ", (unsigned char)buff[x]);

			printf("\n");
        }
    }

    return 0;
}

