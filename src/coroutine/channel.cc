#include "channel.h"

using namespace swoole;

static int channel_onNotify(swReactor *reactor, swEvent *event)
{
    uint64_t notify;
    while (read(SwooleG.chan_pipe->getFd(SwooleG.chan_pipe, 0), &notify, sizeof(notify)) > 0);
    SwooleG.main_reactor->del(SwooleG.main_reactor, SwooleG.chan_pipe->getFd(SwooleG.chan_pipe, 0));
    return 0;
}

static void channel_defer_callback(void *data)
{
    notify_msg_t *msg = (notify_msg_t*) data;
    coroutine_t *co = msg->chan->pop_coroutine(msg->type);
    coroutine_resume(co);
    delete msg;
}

static void channel_pop_timeout(swTimer *timer, swTimer_node *tnode)
{
    timeout_msg_t *msg = (timeout_msg_t *) tnode->data;
    msg->error = true;
    msg->timer = nullptr;
    msg->chan->remove(msg->co);
    coroutine_resume(msg->co);
}

Channel::Channel(size_t _capacity)
{
    capacity = _capacity;
    closed = false;
    notify_producer_count = 0;
    notify_consumer_count = 0;

    if (SwooleG.chan_pipe == NULL)
    {
        SwooleG.chan_pipe = (swPipe *) sw_malloc(sizeof(swPipe));
        if (swPipeNotify_auto(SwooleG.chan_pipe, 1, 1) < 0)
        {
            swError("failed to create eventfd.");
        }
        swReactor_setHandle(SwooleG.main_reactor, SW_FD_CHAN_PIPE, channel_onNotify);
    }
}

void Channel::yield(enum channel_op type)
{
    int _cid = coroutine_get_current_cid();
    if (_cid == -1)
    {
        swError("Socket::yield() must be called in the coroutine.");
    }
    coroutine_t *co = coroutine_get_by_id(_cid);
    if (type == PRODUCER)
    {
        producer_queue.push_back(co);
        swDebug("producer[%d]", coroutine_get_cid(co));
    }
    else
    {
        consumer_queue.push_back(co);
        swDebug("consumer[%d]", coroutine_get_cid(co));
    }
    coroutine_yield(co);
}

void Channel::notify(enum channel_op type)
{
    notify_msg_t *msg = new notify_msg_t;
    msg->chan = this;
    msg->type = type;

    if (type == PRODUCER)
    {
        notify_producer_count++;

    }
    else
    {
        notify_consumer_count++;
    }

    SwooleG.main_reactor->defer(SwooleG.main_reactor, channel_defer_callback, msg);

    int pfd = SwooleG.chan_pipe->getFd(SwooleG.chan_pipe, 0);
    swConnection *_socket = swReactor_get(SwooleG.main_reactor, pfd);
    if (_socket && _socket->events == 0)
    {
        SwooleG.main_reactor->add(SwooleG.main_reactor, pfd, SW_FD_CHAN_PIPE | SW_EVENT_READ);
        uint64_t flag = 1;
        SwooleG.chan_pipe->write(SwooleG.chan_pipe, &flag, sizeof(flag));
    }
}

void* Channel::pop(double timeout)
{
    timeout_msg_t msg;
    msg.error = false;
    if (timeout > 0)
    {
        int msec = (int) (timeout * 1000);
        if (SwooleG.timer.fd == 0)
        {
            swTimer_init (msec);
        }
        msg.chan = this;
        msg.co = coroutine_get_by_id(coroutine_get_current_cid());
        msg.timer = SwooleG.timer.add(&SwooleG.timer, msec, 0, &msg, channel_pop_timeout);
    }
    else
    {
        msg.timer = NULL;
    }
    if (is_empty() || consumer_queue.size() > 0)
    {
        yield(CONSUMER);
    }
    if (msg.error)
    {
        return nullptr;
    }
    if (msg.timer)
    {
        swTimer_del(&SwooleG.timer, msg.timer);
    }
    /**
     * pop data
     */
    void *data = data_queue.front();
    data_queue.pop();
    /**
     * notify producer
     */
    if (producer_queue.size() > 0 && notify_producer_count < producer_queue.size())
    {
        notify(PRODUCER);
    }
    return data;
}

bool Channel::push(void *data)
{
    if (is_full() || producer_queue.size() > 0)
    {
        yield(PRODUCER);
    }
    /**
     * push data
     */
    data_queue.push(data);
    swDebug("push data, count=%ld", length());
    /**
     * notify consumer
     */
    if (consumer_queue.size() > 0 && notify_consumer_count < consumer_queue.size())
    {
        notify(CONSUMER);
    }
    return true;
}

bool Channel::close()
{
    if (closed)
    {
        return false;
    }
    swDebug("closed");
    closed = true;
    while (producer_queue.size() > 0 && notify_producer_count < producer_queue.size())
    {
        notify(PRODUCER);
    }
    while (consumer_queue.size() > 0 && notify_consumer_count < producer_queue.size())
    {
        notify(CONSUMER);
    }
    return true;
}
