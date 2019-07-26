//
// Created by jakub on 7/25/19.
//

#ifndef POINT_CLOUD_TRANSPORT_SIMPLE_PUBLISHER_PLUGIN_H
#define POINT_CLOUD_TRANSPORT_SIMPLE_PUBLISHER_PLUGIN_H

#include "point_cloud_transport/publisher_plugin.h"
#include <boost/scoped_ptr.hpp>

namespace point_cloud_transport {

/**
 * Base class to simplify implementing most plugins to Publisher.
 *
 * This base class vastly simplifies implementing a PublisherPlugin in the common
 * case that all communication with the matching SubscriberPlugin happens over a
 * single ROS topic using a transport-specific message type. SimplePublisherPlugin
 * is templated on the transport-specific message type.
 *
 * A subclass need implement only two methods:
 * - getTransportName() from PublisherPlugin
 * - publish() with an extra PublishFn argument
 *
 * For access to the parameter server and name remappings, use nh().
 *
 * getTopicToAdvertise() controls the name of the internal communication topic.
 * It defaults to \<base topic\>/\<transport name\>.
 *
 */
    template <class M>
    class SimplePublisherPlugin : public PublisherPlugin
    {
    public:
        virtual ~SimplePublisherPlugin() {}

        virtual uint32_t getNumSubscribers() const
        {
            if (simple_impl_) return simple_impl_->pub_.getNumSubscribers();
            return 0;
        }

        virtual std::string getTopic() const
        {
            if (simple_impl_) return simple_impl_->pub_.getTopic();
            return std::string();
        }

        virtual void publish(const sensor_msgs::PointCloud2& message) const
        {
            if (!simple_impl_ || !simple_impl_->pub_) {
                ROS_ASSERT_MSG(false, "Call to publish() on an invalid point_cloud_transport::SimplePublisherPlugin");
                return;
            }

            publish(message, bindInternalPublisher(simple_impl_->pub_));
        }

        virtual void shutdown()
        {
            if (simple_impl_) simple_impl_->pub_.shutdown();
        }

    protected:
        virtual void advertiseImpl(ros::NodeHandle& nh, const std::string& base_topic, uint32_t queue_size,
                                   const SubscriberStatusCallback& user_connect_cb,
                                   const SubscriberStatusCallback& user_disconnect_cb,
                                   const ros::VoidPtr& tracked_object, bool latch)
        {
            std::string transport_topic = getTopicToAdvertise(base_topic);
            ros::NodeHandle param_nh(transport_topic);
            simple_impl_.reset(new SimplePublisherPluginImpl(param_nh));
            simple_impl_->pub_ = nh.advertise<M>(transport_topic, queue_size,
                                                 bindCB(user_connect_cb, &SimplePublisherPlugin::connectCallback),
                                                 bindCB(user_disconnect_cb, &SimplePublisherPlugin::disconnectCallback),
                                                 tracked_object, latch);
        }

        //! Generic function for publishing the internal message type.
        typedef boost::function<void(const M&)> PublishFn;

        /**
         * Publish a point cloud using the specified publish function. Must be implemented by
         * the subclass.
         *
         * The PublishFn publishes the transport-specific message type. This indirection allows
         * SimpleSubscriberPlugin to use this function for both normal broadcast publishing and
         * single subscriber publishing (in subscription callbacks).
         */
        virtual void publish(const sensor_msgs::PointCloud2& message, const PublishFn& publish_fn) const = 0;

        /**
         * Return the communication topic name for a given base topic.
         *
         * Defaults to \<base topic\>/\<transport name\>.
         */
        virtual std::string getTopicToAdvertise(const std::string& base_topic) const
        {
            return base_topic + "/" + getTransportName();
        }

        /**
         * Function called when a subscriber connects to the internal publisher.
         *
         * Defaults to noop.
         */
        virtual void connectCallback(const ros::SingleSubscriberPublisher& pub) {}

        /**
         * Function called when a subscriber disconnects from the internal publisher.
         *
         * Defaults to noop.
         */
        virtual void disconnectCallback(const ros::SingleSubscriberPublisher& pub) {}

        //! Returns the ros::NodeHandle to be used for parameter lookup.
        const ros::NodeHandle& nh() const
        {
            return simple_impl_->param_nh_;
        }

        /**
         * Returns the internal ros::Publisher.
         *
         * TODO: is point_cloud_transport RawPublisher using this?
         * This really only exists so RawPublisher can implement no-copy intraprocess message
         * passing easily.
         */
        const ros::Publisher& getPublisher() const
        {
            ROS_ASSERT(simple_impl_);
            return simple_impl_->pub_;
        }

    private:
        struct SimplePublisherPluginImpl
        {
            SimplePublisherPluginImpl(const ros::NodeHandle& nh)
                    : param_nh_(nh)
            {
            }

            const ros::NodeHandle param_nh_;
            ros::Publisher pub_;
        };

        boost::scoped_ptr<SimplePublisherPluginImpl> simple_impl_;

        typedef void (SimplePublisherPlugin::*SubscriberStatusMemFn)(const ros::SingleSubscriberPublisher& pub);

        /**
         * Binds the user callback to subscriberCB(), which acts as an intermediary to expose
         * a publish(PointCloud2) interface to the user while publishing to an internal topic.
         */
        ros::SubscriberStatusCallback bindCB(const SubscriberStatusCallback& user_cb,
                                             SubscriberStatusMemFn internal_cb_fn)
        {
            ros::SubscriberStatusCallback internal_cb = boost::bind(internal_cb_fn, this, _1);
            if (user_cb)
                return boost::bind(&SimplePublisherPlugin::subscriberCB, this, _1, user_cb, internal_cb);
            else
                return internal_cb;
        }

        /**
         * Forms the ros::SingleSubscriberPublisher for the internal communication topic into
         * a point_cloud_transport::SingleSubscriberPublisher for PointCloud2 messages and passes it
         * to the user subscriber status callback.
         */
        void subscriberCB(const ros::SingleSubscriberPublisher& ros_ssp,
                          const SubscriberStatusCallback& user_cb,
                          const ros::SubscriberStatusCallback& internal_cb)
        {
            // First call the internal callback (for sending setup headers, etc.)
            internal_cb(ros_ssp);

            // Construct a function object for publishing sensor_msgs::PointCloud2 through the
            // subclass-implemented publish() using the ros::SingleSubscriberPublisher to send
            // messages of the transport-specific type.
            typedef void (SimplePublisherPlugin::*PublishMemFn)(const sensor_msgs::PointCloud2&, const PublishFn&) const;
            PublishMemFn pub_mem_fn = &SimplePublisherPlugin::publish;
            PointCloud2PublishFn point_cloud_publish_fn = boost::bind(pub_mem_fn, this, _1, bindInternalPublisher(ros_ssp));

            SingleSubscriberPublisher ssp(ros_ssp.getSubscriberName(), getTopic(),
                                          boost::bind(&SimplePublisherPlugin::getNumSubscribers, this),
                                          point_cloud_publish_fn);
            user_cb(ssp);
        }

        typedef boost::function<void(const sensor_msgs::PointCloud2&)> PointCloud2PublishFn;

        /**
         * Returns a function object for publishing the transport-specific message type
         * through some ROS publisher type.
         *
         * @param pub An object with method void publish(const M&)
         */
        template <class PubT>
        PublishFn bindInternalPublisher(const PubT& pub) const
        {
            // Bind PubT::publish(const Message&) as PublishFn
            typedef void (PubT::*InternalPublishMemFn)(const M&) const;
            InternalPublishMemFn internal_pub_mem_fn = &PubT::publish;
            return boost::bind(internal_pub_mem_fn, &pub, _1);
        }
    };

} //namespace point_cloud_transport


#endif //POINT_CLOUD_TRANSPORT_SIMPLE_PUBLISHER_PLUGIN_H
