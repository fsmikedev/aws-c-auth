/*
 * Copyright 2010-2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/aws_profile.h>
#include <aws/auth/private/credentials_utils.h>
#include <aws/common/process.h>
#include <aws/common/string.h>

#ifdef _MSC_VER
/* allow non-constant declared initializers. */
#    pragma warning(disable : 4204)
#endif

/*
 * Profile provider implementation
 */

AWS_STRING_FROM_LITERAL(s_role_arn_name, "role_arn");
AWS_STRING_FROM_LITERAL(s_role_session_name_name, "role_session_name");
AWS_STRING_FROM_LITERAL(s_credential_source_name, "credential_source");
AWS_STRING_FROM_LITERAL(s_source_profile_name, "source_profile");

static struct aws_byte_cursor s_default_session_name_pfx =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("aws-common-runtime-profile-config");
static struct aws_byte_cursor s_ec2_imds_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Ec2InstanceMetadata");
static struct aws_byte_cursor s_environment_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Environment");

#define MAX_SESSION_NAME_LEN ((size_t)64)

struct aws_credentials_provider_profile_file_impl {
    struct aws_string *config_file_path;
    struct aws_string *credentials_file_path;
    struct aws_string *profile_name;
};

static int s_profile_file_credentials_provider_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_profile_file_impl *impl = provider->impl;
    struct aws_credentials *credentials = NULL;

    /*
     * Parse config file, if it exists
     */
    struct aws_profile_collection *config_profiles =
        aws_profile_collection_new_from_file(provider->allocator, impl->config_file_path, AWS_PST_CONFIG);

    if (config_profiles != NULL) {
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Profile credentials provider successfully built config profile collection from file at (%s)",
            (void *)provider,
            aws_string_c_str(impl->config_file_path));
    } else {
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Profile credentials provider failed to build config profile collection from file at (%s)",
            (void *)provider,
            aws_string_c_str(impl->config_file_path));
    }

    /*
     * Parse credentials file, if it exists
     */
    struct aws_profile_collection *credentials_profiles =
        aws_profile_collection_new_from_file(provider->allocator, impl->credentials_file_path, AWS_PST_CREDENTIALS);

    if (credentials_profiles != NULL) {
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Profile credentials provider successfully built credentials profile collection from file at (%s)",
            (void *)provider,
            aws_string_c_str(impl->credentials_file_path));
    } else {
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Profile credentials provider failed to build credentials profile collection from file at (%s)",
            (void *)provider,
            aws_string_c_str(impl->credentials_file_path));
    }

    /*
     * Merge the (up to) two sources into a single unified profile
     */
    struct aws_profile_collection *merged_profiles =
        aws_profile_collection_new_from_merge(provider->allocator, config_profiles, credentials_profiles);
    if (merged_profiles != NULL) {
        struct aws_profile *profile = aws_profile_collection_get_profile(merged_profiles, impl->profile_name);
        if (profile != NULL) {
            AWS_LOGF_INFO(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Profile credentials provider attempting to pull credentials from profile \"%s\"",
                (void *)provider,
                aws_string_c_str(impl->profile_name));
            credentials = aws_credentials_new_from_profile(provider->allocator, profile);
        } else {
            AWS_LOGF_INFO(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) Profile credentials provider could not find a profile named \"%s\"",
                (void *)provider,
                aws_string_c_str(impl->profile_name));
        }
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) Profile credentials provider failed to merge config and credentials profile collections",
            (void *)provider);
    }

    callback(credentials, user_data);

    /*
     * clean up
     */
    aws_credentials_destroy(credentials);
    aws_profile_collection_destroy(merged_profiles);
    aws_profile_collection_destroy(config_profiles);
    aws_profile_collection_destroy(credentials_profiles);

    return AWS_OP_SUCCESS;
}

static void s_profile_file_credentials_provider_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_profile_file_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    aws_string_destroy(impl->config_file_path);
    aws_string_destroy(impl->credentials_file_path);
    aws_string_destroy(impl->profile_name);

    aws_credentials_provider_invoke_shutdown_callback(provider);

    aws_mem_release(provider->allocator, provider);
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_profile_file_vtable = {
    .get_credentials = s_profile_file_credentials_provider_get_credentials_async,
    .destroy = s_profile_file_credentials_provider_destroy,
};

/* load a purely config/credentials file based provider. */
static struct aws_credentials_provider *s_create_profile_based_provider(
    struct aws_allocator *allocator,
    struct aws_string *credentials_file_path,
    struct aws_string *config_file_path,
    struct aws_string *profile_name) {

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_profile_file_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_profile_file_impl));

    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_profile_file_vtable, impl);
    impl->credentials_file_path = aws_string_clone_or_reuse(allocator, credentials_file_path);
    impl->config_file_path = aws_string_clone_or_reuse(allocator, config_file_path);
    impl->profile_name = aws_string_clone_or_reuse(allocator, profile_name);

    return provider;
}

/* use the selected property that specifies a role_arn to load an STS based provider. */
static struct aws_credentials_provider *s_create_sts_based_provider(
    struct aws_allocator *allocator,
    struct aws_profile_property *role_arn_property,
    struct aws_profile *profile,
    struct aws_string *credentials_file_path,
    struct aws_string *config_file_path,
    const struct aws_credentials_provider_profile_options *options) {
    struct aws_credentials_provider *provider = NULL;

    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "static: profile %s has role_arn property is set to %s, attempting to "
        "create an STS credentials provider.",
        aws_string_c_str(profile->name),
        aws_string_c_str(role_arn_property->value));

    struct aws_profile_property *source_profile_property = aws_profile_get_property(profile, s_source_profile_name);
    struct aws_profile_property *credential_source_property =
        aws_profile_get_property(profile, s_credential_source_name);

    struct aws_profile_property *role_session_name = aws_profile_get_property(profile, s_role_session_name_name);
    char session_name_array[MAX_SESSION_NAME_LEN + 1];
    AWS_ZERO_ARRAY(session_name_array);

    if (role_session_name) {
        size_t to_write = role_session_name->value->len;
        if (to_write > MAX_SESSION_NAME_LEN) {
            AWS_LOGF_WARN(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "static: session_name property is %d bytes long, "
                "but the max is %d. Truncating",
                (int)role_session_name->value->len,
                (int)MAX_SESSION_NAME_LEN);
            to_write = MAX_SESSION_NAME_LEN;
        }
        memcpy(session_name_array, aws_string_bytes(role_session_name->value), to_write);
    } else {
        memcpy(session_name_array, s_default_session_name_pfx.ptr, s_default_session_name_pfx.len);
        snprintf(
            session_name_array + s_default_session_name_pfx.len,
            sizeof(session_name_array) - s_default_session_name_pfx.len,
            "-%d",
            aws_get_pid());
    }

    AWS_LOGF_DEBUG(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: computed session_name as %s", session_name_array);

    struct aws_credentials_provider_sts_options sts_options = {
        .bootstrap = options->bootstrap,
        .role_arn = aws_byte_cursor_from_string(role_arn_property->value),
        .session_name = aws_byte_cursor_from_c_str(session_name_array),
        .duration_seconds = 0,
        .function_table = options->function_table,
    };

    if (source_profile_property) {

        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "static: source_profile set to %s",
            aws_string_c_str(source_profile_property->value));

        sts_options.creds_provider = s_create_profile_based_provider(
            allocator, credentials_file_path, config_file_path, source_profile_property->value);

        if (!sts_options.creds_provider) {
            return NULL;
        }

        provider = aws_credentials_provider_new_sts_cached(allocator, &sts_options);

        aws_credentials_provider_release(sts_options.creds_provider);

        if (!provider) {
            AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: failed to load STS credentials provider");
        }
    } else if (credential_source_property) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "static: credential_source property set to %s",
            aws_string_c_str(credential_source_property->value));

        if (aws_string_eq_byte_cursor_ignore_case(credential_source_property->value, &s_ec2_imds_name)) {
            struct aws_credentials_provider_imds_options imds_options = {
                .bootstrap = options->bootstrap,
                .function_table = options->function_table,
            };

            struct aws_credentials_provider *imds_provider =
                aws_credentials_provider_new_imds(allocator, &imds_options);

            if (!imds_provider) {
                return NULL;
            }

            sts_options.creds_provider = imds_provider;
            provider = aws_credentials_provider_new_sts_cached(allocator, &sts_options);

            aws_credentials_provider_release(imds_provider);

        } else if (aws_string_eq_byte_cursor_ignore_case(credential_source_property->value, &s_environment_name)) {
            struct aws_credentials_provider_environment_options env_options;
            AWS_ZERO_STRUCT(env_options);

            struct aws_credentials_provider *env_provider =
                aws_credentials_provider_new_environment(allocator, &env_options);

            if (!env_provider) {
                return NULL;
            }

            sts_options.creds_provider = env_provider;
            provider = aws_credentials_provider_new_sts_cached(allocator, &sts_options);

            aws_credentials_provider_release(env_provider);
        } else {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "static: invalid credential_source property: %s",
                aws_string_c_str(credential_source_property->value));
        }
    }

    return provider;
}

struct aws_credentials_provider *aws_credentials_provider_new_profile(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_profile_options *options) {

    struct aws_credentials_provider *provider = NULL;
    struct aws_profile_collection *config_profiles = NULL;
    struct aws_profile_collection *credentials_profiles = NULL;
    struct aws_profile_collection *merged_profiles = NULL;
    struct aws_string *credentials_file_path = NULL;
    struct aws_string *config_file_path = NULL;
    struct aws_string *profile_name = NULL;

    credentials_file_path = aws_get_credentials_file_path(allocator, &options->credentials_file_name_override);
    if (!credentials_file_path) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "static: Profile credentials parser failed resolve credentials file path");
        goto on_finished;
    }

    config_file_path = aws_get_config_file_path(allocator, &options->config_file_name_override);
    if (!config_file_path) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: Profile credentials parser failed resolve config file path");
        goto on_finished;
    }

    profile_name = aws_get_profile_name(allocator, &options->profile_name_override);
    if (!profile_name) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: Profile credentials parser failed to resolve profile name");
        goto on_finished;
    }
    config_profiles = aws_profile_collection_new_from_file(allocator, config_file_path, AWS_PST_CONFIG);
    credentials_profiles = aws_profile_collection_new_from_file(allocator, credentials_file_path, AWS_PST_CREDENTIALS);

    if (!(config_profiles || credentials_profiles)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "static: Profile credentials parser could not load or parse"
            " a credentials or config file.");
        goto on_finished;
    }

    merged_profiles = aws_profile_collection_new_from_merge(allocator, config_profiles, credentials_profiles);

    struct aws_profile *profile = aws_profile_collection_get_profile(merged_profiles, profile_name);

    if (!profile) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "static: Profile credentials provider could not load"
            " a profile at %s.",
            aws_string_c_str(profile_name));
        goto on_finished;
    }
    struct aws_profile_property *role_arn_property = aws_profile_get_property(profile, s_role_arn_name);

    if (role_arn_property) {
        provider = s_create_sts_based_provider(
            allocator, role_arn_property, profile, credentials_file_path, config_file_path, options);
    } else {
        provider = s_create_profile_based_provider(allocator, credentials_file_path, config_file_path, profile_name);
    }

on_finished:
    if (config_profiles) {
        aws_profile_collection_destroy(config_profiles);
    }

    if (credentials_profiles) {
        aws_profile_collection_destroy(credentials_profiles);
    }

    if (merged_profiles) {
        aws_profile_collection_destroy(merged_profiles);
    }

    aws_string_destroy(credentials_file_path);
    aws_string_destroy(config_file_path);
    aws_string_destroy(profile_name);

    if (provider) {
        provider->shutdown_options = options->shutdown_options;
    }

    return provider;
}
