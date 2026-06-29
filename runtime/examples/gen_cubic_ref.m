function gen_cubic_ref()
    % MATLAB interpn('cubic') = Keys cubic convolution (a=-0.5) reference for v13-f bicubic.
    n = 6;
    [I, J] = ndgrid(0:n-1, 0:n-1);
    V = I.^2 + J.^2 + I.*J - 2*I + 0.5*J + 1;   % quadratic field
    qx = [1.3, 2.7, 3.4, 2.1];
    qy = [1.7, 2.2, 3.8, 1.4];
    vq = interpn(I, J, V, qx, qy, 'cubic');
    fprintf('Vc '); fprintf('%.17g ', V');  fprintf('\n');   % V' => row-major (i outer, j inner)
    fprintf('cubicq '); fprintf('%.17g ', vq); fprintf('\n');
end
