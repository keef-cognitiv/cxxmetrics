#ifndef CXXMETRICS_METRICS_REGISTRY_HPP
#define CXXMETRICS_METRICS_REGISTRY_HPP

#include <mutex>
#include <memory>
#include "metric_path.hpp"
#include "tag_collection.hpp"
#include "counter.hpp"
#include "ewma.hpp"

namespace cxxmetrics
{
template<typename TRepository>
class metrics_registry;

namespace internal
{

class registered_snapshot_visitor_builder
{
public:
    virtual std::size_t visitor_size() const = 0;
    virtual void construct(snapshot_visitor* location, const tag_collection& collection) = 0;
};

template<typename TVisitor>
class invokable_snapshot_visitor_builder : public registered_snapshot_visitor_builder
{
    TVisitor visitor_;
    using visitor_type = decltype(std::bind(std::declval<TVisitor>(), std::declval<tag_collection&>(), std::placeholders::_1));
public:
    invokable_snapshot_visitor_builder(TVisitor&& visitor) :
            visitor_(std::forward<TVisitor>(visitor))
    { }

    std::size_t visitor_size() const override
    {
        return sizeof(invokable_snapshot_visitor<visitor_type>);
    }

    void construct(snapshot_visitor* location, const tag_collection& collection) override
    {
        using namespace std::placeholders;
        new (location) invokable_snapshot_visitor<visitor_type>(std::bind(visitor_, std::forward<const tag_collection&>(collection), _1));
    }
};

}

/**
 * \brief an exception thrown when a registry action is performed with the wrong metric type
 */
class metric_type_mismatch : public std::exception
{
    std::string existing_;
    std::string desired_;
public:
    metric_type_mismatch(std::string existing_type, std::string desired_type) :
            existing_(std::move(existing_type)),
            desired_(std::move(desired_type))
    { }

    const char* what() const noexcept override
    {
        return "The existing registered metric did not match the desired type";
    }

    /**
     * \brief get the type of metric that already existed in the registry
     */
    std::string existing_metric_type() const
    {
        return existing_;
    }

    /**
     * \brief get the type that was desired but was mismatched in the registry
     */
    std::string desired_metric_type() const
    {
        return desired_;
    }
};

/**
 * \brief The root metric that's registered in a repository
 *
 * A metric is registered in the repository by its path. However, the paths only describe the
 * metric metadata and a container of the actual metrics by their tag. publishers access the metrics
 * by their registered_metric. From there, they can publish per tagset metrics or summaries or both.
 *
 * Publishers are also able to store publisher specific data in the registered metric.
 */
class basic_registered_metric
{
    std::string type_;

    template<typename TMetricType, typename... TConstructorArgs>
    TMetricType& tagged(const tag_collection& tags, TConstructorArgs&&... args)
    {
        auto builder = [&]() -> TMetricType {
            return TMetricType(std::forward<TConstructorArgs>(args)...);
        };

        invokable_metric_builder<typename std::decay<decltype(builder)>::type> metricbuilder(std::move(builder));
        return *static_cast<TMetricType*>(this->child(tags, &metricbuilder));
    }

    template<typename TRepository>
    friend class metrics_registry;
protected:
    template<typename TMetricType>
    struct basic_metric_builder
    {
        virtual ~basic_metric_builder() = default;
        virtual TMetricType build() = 0;
    };

    template<typename TBuilder>
    class invokable_metric_builder final : public basic_metric_builder<decltype(std::declval<TBuilder>()())>
    {
        TBuilder builder_;
    public:
        invokable_metric_builder(TBuilder&& builder) :
                builder_(std::forward<TBuilder>(builder))
        { }

        decltype(builder_()) build() override
        {
            return builder_();
        }
    };

    virtual void visit_each(internal::registered_snapshot_visitor_builder& builder) = 0;
    virtual void aggregate_all(snapshot_visitor& visitor) = 0;
    virtual internal::metric* child(const tag_collection& tags, void* metricbuilder) = 0;

public:
    basic_registered_metric(const std::string& type) :
            type_(type)
    { }

    virtual ~basic_registered_metric() = default;

    /**
     * \brief Visits all of the metrics with their tag values, calling a handler for each
     *
     * The handler should accept 2 arguments: the first is a tag_collection, which will be the
     * tags associated to the metric. The second will be the actual metric snapshot value
     *
     * \tparam THandler the handler type which ought to be auto-deduced from the parameter
     * \param handler the instance of the handler which will be called for each of the metrics
     */
    template<typename THandler>
    void visit(THandler&& handler) {
        internal::invokable_snapshot_visitor_builder<THandler> builder(std::forward<THandler>(handler));
        this->visit_each(builder);
    }

    /**
     * \brief Aggregates all of the metrics and their different tag values into a single metric
     *
     * The Handler will be called with the aggregated snapshot of all the tagged permutations of the metric
     *
     * \tparam THandler the visitor handler that will receive a single call with the aggregated snapshot
     *
     * \param handler the instance of the handler
     */
    template<typename THandler>
    void aggregate(THandler&& handler) {
        invokable_snapshot_visitor<THandler> visitor(std::forward<THandler>(handler));
        this->aggregate_all(visitor);
    }

    /**
     * \brief Get the type of metric registered
     */
    virtual std::string type() const { return type_; }
};

/**
 * \brief the specialized root metric that will be the real types registered in the repository
 *
 * @tparam TMetricType the type of metric registered in the repository
 */
template<typename TMetricType>
class registered_metric : public basic_registered_metric
{
    std::unordered_map<tag_collection, TMetricType> metrics_;
    std::mutex lock_;

protected:
    void visit_each(internal::registered_snapshot_visitor_builder& builder) override;
    void aggregate_all(snapshot_visitor& visitor) override;
    internal::metric* child(const tag_collection& tags, void* metricbuilder) override;

public:
    registered_metric(const std::string& metric_type_name) :
            basic_registered_metric(metric_type_name)
    { }
};

template<typename TMetricType>
void registered_metric<TMetricType>::visit_each(cxxmetrics::internal::registered_snapshot_visitor_builder &builder)
{
    std::lock_guard<std::mutex> lock(lock_);
    for (auto& p : metrics_)
    {
        auto sz = builder.visitor_size() + sizeof(std::max_align_t);
        void* ptr = alloca(sz);

        std::align(sizeof(std::max_align_t), sz, ptr, sz);
        auto loc = reinterpret_cast<snapshot_visitor*>(ptr);
        builder.construct(loc, p.first);
        try
        {
            loc->visit(p.second.snapshot());
        }
        catch (...)
        {
            loc->~snapshot_visitor();
            throw;
        }

        loc->~snapshot_visitor();
    }
}

template<typename TMetricType>
void registered_metric<TMetricType>::aggregate_all(snapshot_visitor &visitor)
{
    std::unique_lock<std::mutex> lock(lock_);

    auto itr = metrics_.begin();
    if (itr == metrics_.end())
        return;

    auto result = itr->second.snapshot();
    for (++itr; itr != metrics_.end(); ++itr)
    {
        result.merge(itr->second.snapshot());
    }

    lock.unlock();
    visitor.visit(result);
}

template<typename TMetricType>
internal::metric* registered_metric<TMetricType>::child(const cxxmetrics::tag_collection &tags, void* metricbuilder)
{
    std::lock_guard<std::mutex> lock(lock_);
    auto res = metrics_.find(tags);

    if (res != metrics_.end())
        return &res->second;

    return &metrics_.emplace(tags, static_cast<basic_metric_builder<TMetricType>*>(metricbuilder)->build()).first->second;
}

/**
 * \brief The default metric repository that registers metrics in a standard unordered map with a mutex lock
 */
template<typename TAlloc>
class basic_default_repository
{
    std::unordered_map<metric_path, std::unique_ptr<basic_registered_metric>, std::hash<metric_path>, std::equal_to<metric_path>, TAlloc> metrics_;
    std::mutex lock_;
public:
    basic_default_repository() = default;

    template<typename TMetricPtrBuilder>
    basic_registered_metric& get_or_add(const metric_path& name, const TMetricPtrBuilder& builder);

    template<typename THandler>
    void visit(THandler&& handler);

};

template<typename TAlloc>
template<typename TMetricPtrBuilder>
basic_registered_metric& basic_default_repository<TAlloc>::get_or_add(const metric_path& name, const TMetricPtrBuilder& builder)
{
    std::lock_guard<std::mutex> lock(lock_);
    auto existing = metrics_.find(name);

    if (existing == metrics_.end())
    {
        auto ptr = builder();
        return *metrics_.emplace(name, std::move(ptr)).first->second;
    }

    return *existing->second;
}

template<typename TAlloc>
template<typename THandler>
void basic_default_repository<TAlloc>::visit(THandler&& handler)
{
    std::lock_guard<std::mutex> lock(lock_);
    for (auto& pair : metrics_)
        handler(pair.first, *pair.second);
}

using default_repository = basic_default_repository<std::allocator<std::pair<metric_path, basic_registered_metric>>>;

/**
 * \brief The registry where metrics are registered
 *
 * \tparam TRepository the type of registry to store the metric hierarchy in
 */
template<typename TRepository = default_repository>
class metrics_registry
{
    TRepository repo_;

    template<typename TMetricType>
    registered_metric<TMetricType>& get(const metric_path& path);

    template<typename TMetricType, typename... TConstructorArgs>
    TMetricType& get(const metric_path& path, const tag_collection& tags, TConstructorArgs&&... args);

public:
    /**
     * \brief Construct the registry with the arguments being passed to the underlying repository
     */
    template<typename... TRepoArgs>
    metrics_registry(TRepoArgs&&... args);

    metrics_registry(const metrics_registry&) = default;
    metrics_registry(metrics_registry&& other) noexcept :
            repo_(std::move(other.repo_))
    { }
    ~metrics_registry() = default;

    /**
     *  \brief Run a visitor on all of the registered metrics
     *
     *  This is particularly useful for metric publishers. The handler will be called with
     *  a signature of
     *
     *  handler(const metric_path&, basic_registered_metric&)
     *
     *  The basic_registered_metric is where publishers can get metric-specific publisher
     *  data and where you can use another handler to either aggregate the metric values
     *  across all sets of tags or visit each of the tagged permutations of the metric.
     *
     * \param handler the handler to execute per metric registration
     */
    template<typename THandler>
    void visit_registered_metrics(THandler&& handler);

    /**
     * \brief Get the registered counter or register a new one with the given path and tags
     *
     * If a metric is already registered but it's not a counter of the specified type, this
     * will throw an exception of type metric_type_mismatch
     *
     * \throws metric_type_mismatch if there is already a registered metric at the path of a different type
     *
     * \tparam TCount the type of counter
     *
     * \param name the name of the metric to get
     * \param initialValue the initial value for the counter (ignored if counter already exists)
     * \param tags the tags for the permutation being sought
     *
     * \return the counter at the path specified with the tags specified
     */
    template<typename TCount = int64_t>
    cxxmetrics::counter<TCount>& counter(const metric_path& name, TCount&& initialValue, const tag_collection& tags = tag_collection());

    /**
     * \brief Another overload for getting a counter without an initial value
     */
    template<typename TCount = int64_t>
    cxxmetrics::counter<TCount>& counter(const metric_path& name, const tag_collection& tags = tag_collection())
    {
        return this->template counter<TCount>(name, 0, tags);
    }

    /**
     * \brief Get the registered exponential moving average or register a new one with the given path and tags
     *
     * \tparam TValue the type of data in the ewma
     *
     * \param name the name of the metric to get
     * \param window the ewma full window outside of which values are fully decayed (ignored if the ewma already exists)
     * \param interval the window in which values are summed (ignored if the ewma already exists)
     * \param tags the tags for the permutation being sought.
     *
     * \return the ewma at the path specified with the tags specified
     */
    template<typename TValue = double>
    cxxmetrics::ewma<TValue>& ewma(const metric_path& name,
                       const std::chrono::steady_clock::duration& window,
                       const std::chrono::steady_clock::duration& interval = std::chrono::seconds(5),
                       const tag_collection& tags = tag_collection());
};

template<typename TRepository>
template<typename... TRepoArgs>
metrics_registry<TRepository>::metrics_registry(TRepoArgs &&... args) :
        repo_(std::forward<TRepoArgs>(args)...)
{ }

template<typename TRepository>
template<typename TMetricType>
registered_metric<TMetricType>& metrics_registry<TRepository>::get(const metric_path& path)
{
    static const std::string mtype = internal::metric_default_value<TMetricType>().metric_type();
    auto& l = repo_.get_or_add(path, [tn = mtype]() { return std::make_unique<registered_metric<TMetricType>>(tn); });

    if (l.type() != mtype)
        throw metric_type_mismatch(l.type(), mtype);

    return static_cast<registered_metric<TMetricType>&>(l);
}

template<typename TRepository>
template<typename TMetricType, typename... TConstructorArgs>
TMetricType& metrics_registry<TRepository>::get(const metric_path& path, const tag_collection& tags, TConstructorArgs&&... args)
{
    auto& r = get<TMetricType>(path);
    return r.template tagged<TMetricType>(tags, std::forward<TConstructorArgs>(args)...);
}

template<typename TRepository>
template<typename THandler>
void metrics_registry<TRepository>::visit_registered_metrics(THandler &&handler)
{
    repo_.visit(std::forward<THandler>(handler));
}

template<typename TRepository>
template<typename TCount>
counter<TCount>& metrics_registry<TRepository>::counter(const metric_path& name, TCount&& initialValue, const tag_collection& tags)
{
    return get<cxxmetrics::counter<TCount>>(name, tags, std::forward<TCount>(initialValue));
}

template<typename TRepository>
template<typename TValue>
ewma<TValue> & metrics_registry<TRepository>::ewma(const metric_path& name,
        const std::chrono::steady_clock::duration& window,
        const std::chrono::steady_clock::duration& interval,
        const tag_collection& tags)
{
    return get<cxxmetrics::ewma<TValue>>(name, tags, window, interval);
}

}

#endif //CXXMETRICS_METRICS_REGISTRY_HPP
