void cleanup_client(client_context *ctx) {
    if (ctx->active_sock_fd >= 0) {
        printf("Closing connection and exiting...\n");
        close(ctx->active_sock_fd);
        ctx->active_sock_fd = -1;
    }
}

//add quit

//add print usage